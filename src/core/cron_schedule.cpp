// =============================================================================
// src/core/cron_schedule.cpp — cron expression parsing + next-run arithmetic
// =============================================================================

#include "cron_schedule.h"
#include <sstream>
#include <stdexcept>
#include <vector>

namespace funes::cron {

namespace {

// Parses one comma-separated cron field ("*", "*/N", "a", "a-b", "a-b/N")
// into the values it selects, clamped to [lo, hi].
std::set<int> parse_field(const std::string& field, int lo, int hi, const char* field_name) {
    std::set<int> out;
    std::stringstream ss(field);
    std::string part;

    auto fail = [&](const std::string& why) {
        throw std::runtime_error("invalid cron " + std::string(field_name) +
                                 " field '" + field + "': " + why);
    };

    while (std::getline(ss, part, ',')) {
        if (part.empty()) fail("empty term");

        int step = 1;
        auto slash = part.find('/');
        std::string range = part;
        if (slash != std::string::npos) {
            range = part.substr(0, slash);
            try {
                step = std::stoi(part.substr(slash + 1));
            } catch (...) {
                fail("bad step value");
            }
            if (step <= 0) fail("step must be positive");
        }

        int start = 0, end = 0;
        if (range == "*") {
            start = lo;
            // day-of-week's own upper bound (7) exists only to accept an
            // explicit second-Sunday alias below — "*" itself must span
            // 0-6, not 0-7 (which would insert a spurious extra Sunday).
            end = (std::string(field_name) == "day-of-week") ? 6 : hi;
        } else {
            auto dash = range.find('-');
            try {
                if (dash != std::string::npos) {
                    start = std::stoi(range.substr(0, dash));
                    end   = std::stoi(range.substr(dash + 1));
                } else {
                    start = end = std::stoi(range);
                }
            } catch (...) {
                fail("not a number");
            }
            // Cron's day-of-week allows 7 as a second Sunday, for explicit
            // values only ("*" is handled above and never reaches here).
            if (std::string(field_name) == "day-of-week") {
                if (start == 7) start = 0;
                if (end == 7) end = 0;
            }
        }

        if (start < lo || start > hi || end < lo || end > hi || start > end)
            fail("value out of range [" + std::to_string(lo) + "," + std::to_string(hi) + "]");

        for (int v = start; v <= end; v += step) out.insert(v);
    }

    if (out.empty()) fail("no values selected");
    return out;
}

} // namespace

CronExpr parse_cron_expr(const std::string& expr) {
    std::istringstream iss(expr);
    std::vector<std::string> fields;
    std::string tok;
    while (iss >> tok) fields.push_back(tok);

    if (fields.size() != 5)
        throw std::runtime_error("cron expression must have exactly 5 fields "
                                 "(minute hour day-of-month month day-of-week), got '" +
                                 expr + "'");

    CronExpr out;
    out.minutes       = parse_field(fields[0], 0, 59, "minute");
    out.hours         = parse_field(fields[1], 0, 23, "hour");
    out.days_of_month = parse_field(fields[2], 1, 31, "day-of-month");
    out.months        = parse_field(fields[3], 1, 12, "month");
    out.days_of_week  = parse_field(fields[4], 0, 7,  "day-of-week");

    out.any_day_of_month = (fields[2] == "*");
    out.any_day_of_week  = (fields[4] == "*");
    return out;
}

time_t next_run_after(const CronExpr& expr, time_t from) {
    // Round up to the next full minute — a match at `from`'s own minute
    // (e.g. re-evaluating right after a run finished) must not fire again
    // for the same minute it already ran in.
    time_t t = from - (from % 60) + 60;

    // A year of minutes, generously bounded — if nothing matches by then the
    // expression selects an impossible combination (e.g. Feb 30) and this
    // is a bug in the caller's input, not a schedule to wait on forever.
    constexpr int kMaxMinutes = 366 * 24 * 60;

    for (int i = 0; i < kMaxMinutes; ++i) {
        struct tm lt;
        localtime_r(&t, &lt);

        const bool month_ok  = expr.months.count(lt.tm_mon + 1) > 0;
        const bool hour_ok   = expr.hours.count(lt.tm_hour) > 0;
        const bool minute_ok = expr.minutes.count(lt.tm_min) > 0;

        const bool dom_ok = expr.days_of_month.count(lt.tm_mday) > 0;
        const bool dow_ok = expr.days_of_week.count(lt.tm_wday) > 0;
        // OR when both are restricted, otherwise whichever is restricted
        // (or "*" AND "*", which is trivially true either way).
        const bool day_ok = (expr.any_day_of_month || expr.any_day_of_week)
                                ? (dom_ok && dow_ok)
                                : (dom_ok || dow_ok);

        if (month_ok && day_ok && hour_ok && minute_ok) return t;
        t += 60;
    }

    throw std::runtime_error("cron expression never matches within a year — "
                             "check for an impossible date (e.g. day-of-month 30 in February)");
}

} // namespace funes::cron
