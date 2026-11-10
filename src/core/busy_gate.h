// =============================================================================
// src/core/busy_gate.h — background work yields to people
// =============================================================================
// The link backfill was off by default for one reason: it is one model call
// per candidate pair on the GPU the chat path needs, and it had no way to know
// whether someone was waiting. This is that way. Two signals, both cheap:
//
//   - Funes's own in-flight agent runs. It knows when the model is busy
//     because it is the one keeping it busy; every FunesAgent::run (a chat,
//     a cron job, a delegated sub-run) holds a RunInFlight for its duration.
//   - Linux pressure stall information (/proc/pressure/{cpu,io}): the share of
//     the last ten seconds in which some task was stalled waiting. Catches the
//     load Funes did not cause — a rebuild, a backup, another process.
//
// A background pass calls wait_until_quiet() before *each* model call, not
// once per run, so a chat that arrives mid-pass waits for at most one
// judgment instead of the whole batch.

#pragma once
#include <chrono>
#include <string>

namespace funes::load {

// Agent runs in progress, process-wide.
int runs_in_flight();

// RAII: counts one run in flight for its lifetime.
class RunInFlight {
public:
    RunInFlight();
    ~RunInFlight();
    RunInFlight(const RunInFlight&) = delete;
    RunInFlight& operator=(const RunInFlight&) = delete;
};

// "some avg10" from a PSI file such as /proc/pressure/cpu, in percent.
// Negative when the file is missing or unparsable (kernel without PSI, or a
// container that hides it) — "cannot tell" must not read as "busy forever".
double psi_some_avg10(const std::string& path);

struct QuietOptions {
    double max_cpu_pressure = 20.0;   // percent, some avg10
    double max_io_pressure  = 20.0;
    std::chrono::seconds poll{5};
    // Give up waiting after this long and run anyway: a host that is never
    // quiet should get a slow backfill, not none at all.
    std::chrono::seconds max_wait{std::chrono::minutes(30)};
    std::string cpu_path = "/proc/pressure/cpu";
    std::string io_path  = "/proc/pressure/io";
};

// True when no agent run is in flight and neither pressure is over its
// threshold (an unreadable pressure counts as under).
bool is_quiet(const QuietOptions& opt = {});

// Blocks until is_quiet() or opt.max_wait has passed. Returns whether it was
// quiet (false = gave up waiting).
bool wait_until_quiet(const QuietOptions& opt = {});

} // namespace funes::load
