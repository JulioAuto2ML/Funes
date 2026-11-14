// =============================================================================
// src/core/probes.h — noticing that something broke, without a model
// =============================================================================
// Self-healing plan, phase H0 (docs/self-healing-plan.md). A probe is a check
// that runs on a timer and says what is wrong in one line. Two kinds:
//
//   A script probe is a library script (scriptlib/) whose manifest has a
//   `probe:` block —
//       probe: {every: 300, fail_after: 2}
//       output: {format: json}
//   It runs in the same sandbox `run_script` gives it (its `access:` and
//   nothing else) and prints {"checks": [{"name", "ok", "summary",
//   "severity"?, "evidence"?}]}: one probe, several checks (both bridges, both
//   models), each its own incident.
//
//   A built-in probe is a function in this binary, for what only the server
//   knows: whether its own cron jobs failed.
//
// Every check result goes to the IncidentStore; the runner then sends what is
// pending, unless it is quiet hours and nothing is critical. A probe that
// cannot run (exit ≠ 0, a timeout, output in the wrong shape) is itself an
// incident, "<probe>:(run)" — a broken check must not look like a clean bill
// of health.
// =============================================================================

#pragma once
#include "incidents.h"
#include "json.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace funes::probes {

using incidents::CheckResult;
using incidents::Incident;

struct BuiltinProbe {
    std::string name;
    int every_seconds = 300;
    int fail_after = 1;
    std::function<std::vector<CheckResult>()> run;
};

// A script probe as the runner sees it, read from its manifest.
struct ScriptProbe {
    std::string name;
    int every_seconds = 300;
    int fail_after = 2;
};

struct Options {
    // Which script probes exist right now (re-read every tick, so a new
    // manifest is picked up without a restart).
    std::function<std::vector<ScriptProbe>()> list_scripts;
    // Runs one; "" and `out` set on success, else why it could not run.
    std::function<std::string(const std::string& name, nlohmann::json& out)> run_script;
    std::vector<BuiltinProbe> builtins;
    // Sends one notice; false when it could not (it stays pending).
    std::function<bool(const Incident&)> notify;
    // "23-8": no notices from 23:00 to 07:59 local time, except critical
    // ones. Empty: none.
    std::string quiet_hours;
    // Local hour now, 0–23. Injected for the tests.
    std::function<int(int64_t now)> local_hour;
};

struct Report {
    int ran = 0;
    int opened = 0;
    int resolved = 0;
    int noticed = 0;
};

class Runner {
public:
    Runner(incidents::IncidentStore& store, Options opt);
    Report tick(int64_t now);

private:
    void record(const std::string& probe, const std::vector<CheckResult>& checks,
                int fail_after, int64_t now, Report& r);
    void run_failed(const std::string& probe, const std::string& why, int fail_after,
                    int64_t now, Report& r);
    void send_notices(int64_t now, Report& r);

    incidents::IncidentStore& store_;
    Options opt_;
    std::map<std::string, int64_t> last_run_;
    int64_t next_notice_try_ = 0;
};

// {"checks": [...]} → results keyed "<probe>:<name>". "" or why the shape is wrong.
std::string parse_checks(const std::string& probe, const nlohmann::json& out,
                         std::vector<CheckResult>& into);

bool in_quiet_hours(const std::string& spec, int hour);

// The WhatsApp text for one pending notice, in `locale` ("es" or English).
std::string notice_text(const Incident& i, const std::string& locale);

} // namespace funes::probes
