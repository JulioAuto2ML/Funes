// =============================================================================
// tests/test_cron_schedule.cpp — cron expression parsing + next-run arithmetic
// =============================================================================
// Pure functions, no DB, no live scheduler — see core/cron_schedule.h.

#include "cron_schedule.h"
#include <ctime>
#include <iostream>

using namespace funes::cron;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

time_t make_time(int year, int mon, int day, int hour, int min) {
    struct tm t{};
    t.tm_year  = year - 1900;
    t.tm_mon   = mon - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = min;
    t.tm_isdst = -1;
    return mktime(&t);
}

// Walks forward a day at a time from `start` until it finds one that falls
// on `target_wday` (0=Sunday), so weekday-dependent tests don't have to
// hardcode which day of the week any particular calendar date lands on.
time_t find_wday(time_t start, int target_wday) {
    time_t t = start;
    for (int i = 0; i < 8; ++i) {
        struct tm lt;
        localtime_r(&t, &lt);
        if (lt.tm_wday == target_wday) return t;
        t += 86400;
    }
    throw std::runtime_error("find_wday: not found within a week");
}

time_t at_time_of_day(time_t day_anchor, int hour, int min) {
    struct tm lt;
    localtime_r(&day_anchor, &lt);
    return make_time(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, hour, min);
}

} // namespace

int test_parse_valid() {
    CronExpr any = parse_cron_expr("* * * * *");
    CHECK(any.minutes.size() == 60);
    CHECK(any.hours.size() == 24);
    // The day-7-folds-to-Sunday alias must not leak into the wildcard
    // expansion (a real regression: it used to collapse "*" to {0}).
    CHECK(any.days_of_week.size() == 6 + 1);
    CHECK(any.days_of_week.count(0) && any.days_of_week.count(6));
    CHECK(!any.days_of_week.count(7));

    CronExpr daily = parse_cron_expr("0 9 * * *");
    CHECK(daily.minutes.size() == 1 && daily.minutes.count(0));
    CHECK(daily.hours.size() == 1 && daily.hours.count(9));
    CHECK(daily.any_day_of_month && daily.any_day_of_week);

    CronExpr step = parse_cron_expr("*/15 * * * *");
    CHECK(step.minutes == std::set<int>({0, 15, 30, 45}));

    CronExpr weekdays = parse_cron_expr("0 9 * * 1-5");
    CHECK(weekdays.days_of_week == std::set<int>({1, 2, 3, 4, 5}));
    CHECK(!weekdays.any_day_of_week);
    CHECK(weekdays.any_day_of_month);

    // 7 is an accepted alias for Sunday (0) on explicit values.
    CronExpr sunday_alias = parse_cron_expr("0 0 * * 7");
    CHECK(sunday_alias.days_of_week == std::set<int>({0}));

    CronExpr list = parse_cron_expr("1,15,30 * * * *");
    CHECK(list.minutes == std::set<int>({1, 15, 30}));

    return 0;
}

int test_parse_rejects_invalid() {
    auto rejects = [](const std::string& expr) {
        try {
            parse_cron_expr(expr);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    CHECK(rejects("0 9 * *"));           // only 4 fields
    CHECK(rejects("0 9 * * * *"));       // 6 fields
    CHECK(rejects("60 * * * *"));        // minute out of range
    CHECK(rejects("* 24 * * *"));        // hour out of range
    CHECK(rejects("* * 32 * *"));        // day-of-month out of range
    CHECK(rejects("* * * 13 *"));        // month out of range
    CHECK(rejects("* * * * 8"));         // day-of-week out of range (7 is the max alias)
    CHECK(rejects("abc * * * *"));       // not a number
    CHECK(rejects("*/0 * * * *"));       // step must be positive
    CHECK(rejects(""));                  // empty

    return 0;
}

int test_next_run_daily() {
    // Before 9am: fires later the same day.
    CronExpr daily = parse_cron_expr("0 9 * * *");
    time_t from   = make_time(2026, 3, 10, 8, 0);
    time_t expect = make_time(2026, 3, 10, 9, 0);
    CHECK(next_run_after(daily, from) == expect);

    // After 9am: rolls to the next day.
    from   = make_time(2026, 3, 10, 9, 30);
    expect = make_time(2026, 3, 11, 9, 0);
    CHECK(next_run_after(daily, from) == expect);

    // Exactly at 9:00:00 itself: must not fire again for the same minute —
    // next_run_after is exclusive of `from`.
    from   = make_time(2026, 3, 10, 9, 0);
    expect = make_time(2026, 3, 11, 9, 0);
    CHECK(next_run_after(daily, from) == expect);

    return 0;
}

int test_next_run_step() {
    CronExpr every15 = parse_cron_expr("*/15 * * * *");
    time_t from   = make_time(2026, 3, 10, 8, 5);
    time_t expect = make_time(2026, 3, 10, 8, 15);
    CHECK(next_run_after(every15, from) == expect);

    // Rolls into the next hour cleanly.
    from   = make_time(2026, 3, 10, 8, 50);
    expect = make_time(2026, 3, 10, 9, 0);
    CHECK(next_run_after(every15, from) == expect);

    return 0;
}

int test_next_run_weekday_or_semantics() {
    // day-of-month is "*" (unrestricted) and day-of-week is restricted to
    // Mon-Fri, so only the weekday restriction applies (the AND/OR rule
    // degenerates to a plain AND when one side is unrestricted).
    CronExpr weekdays = parse_cron_expr("0 9 * * 1-5");

    time_t saturday = find_wday(make_time(2026, 1, 1, 0, 0), 6);
    time_t from      = at_time_of_day(saturday, 10, 0);  // Saturday 10am, past 9am

    time_t monday   = saturday + 2 * 86400;
    time_t expect   = at_time_of_day(monday, 9, 0);

    CHECK(next_run_after(weekdays, from) == expect);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_parse_valid();
    rc |= test_parse_rejects_invalid();
    rc |= test_next_run_daily();
    rc |= test_next_run_step();
    rc |= test_next_run_weekday_or_semantics();
    if (rc == 0) std::cout << "test_cron_schedule: all tests passed\n";
    return rc;
}
