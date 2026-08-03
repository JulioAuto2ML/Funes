// =============================================================================
// src/core/cron_schedule.h — standard 5-field cron expression evaluation
// =============================================================================
// Pure functions, no DB, no clock of their own — same shape as harvest.h's
// pure candidate-selection core, so the interesting logic (parsing, next-run
// arithmetic) is unit-testable without a live scheduler or a mocked clock.
//
// Supports the subset of cron syntax actually in use: "*", single values,
// comma lists ("1,15"), ranges ("1-5"), and step values ("*/15", "1-30/5").
// Evaluated against the server process's local time, matching how OS cron
// itself behaves on the host — no timezone database, no DST correction.
// =============================================================================

#pragma once
#include <ctime>
#include <set>
#include <string>

namespace funes::cron {

struct CronExpr {
    std::set<int> minutes;       // 0-59
    std::set<int> hours;         // 0-23
    std::set<int> days_of_month; // 1-31
    std::set<int> months;        // 1-12
    std::set<int> days_of_week;  // 0-6, 0=Sunday (7 in the input normalizes to 0)

    // Standard cron quirk: day-of-month and day-of-week are AND'd with the
    // rest but OR'd with each other — *if* both are restricted ("run on the
    // 1st OR on a Monday"). Tracked separately from the expanded sets above
    // because "*" and an explicit full range ("1-31") expand to the same
    // set but mean different things for this OR rule.
    bool any_day_of_month = true;
    bool any_day_of_week  = true;
};

// Parses a standard 5-field expression ("m h dom mon dow"). Throws
// std::runtime_error with a message naming the bad field on malformed input.
CronExpr parse_cron_expr(const std::string& expr);

// The next time at or after `from` (exclusive of `from` itself) that matches
// `expr`, in local time. Walks forward minute by minute — a year is ~525k
// steps, trivial at this call frequency (once per scheduled job, not per
// poll tick).
//
// day_of_month and day_of_week are OR'd when both are restricted (non-"*"),
// matching standard cron semantics: "run on the 1st OR on a Monday".
time_t next_run_after(const CronExpr& expr, time_t from);

} // namespace funes::cron
