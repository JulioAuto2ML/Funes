// =============================================================================
// src/core/incidents.h — what is broken right now, and who has been told
// =============================================================================
// Self-healing plan, phase H0 (docs/self-healing-plan.md). Probes (probes.h)
// report checks — "the LinkedIn token has 2 days left", "bridge :8091 does not
// answer" — and this store turns a stream of results into incidents: one open
// incident per failing check, however many times the check runs while it
// fails, closed when it passes again.
//
// Before it, a failure was in a log until a person read the log. On
// 2026-11-12 the LinkedIn token expired and every post failed for a day with
// "ERROR: LinkedIn access token has expired" in post_log.txt; on 2026-11-14
// yoda lost the internet when anakin's uplink went down, and the first sign
// was a failed `git pull` in a Claude session.
//
// Telling someone is a separate, pending step (`pending_notices` /
// `mark_noticed`), not part of `observe`: a notice that could not be sent
// (quiet hours, the bridge being the thing that broke) stays pending and goes
// out on a later tick instead of being lost. An incident that opened and
// closed before anyone was told is told once, as both.
//
// Its own connection on the memory database, like host_events and users:
// nothing here is anyone's memory.
// =============================================================================

#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace funes::incidents {

// What one probe check said, once.
struct CheckResult {
    std::string key;        // "<probe>:<check>", e.g. "probe_services:llm"
    bool        ok = true;
    std::string severity;   // "warning" | "critical"
    std::string summary;    // one line, for a person
    std::string evidence;   // what the check saw (log text: hostile, quoted, capped)
};

struct Incident {
    int64_t     id = 0;
    std::string key;
    std::string severity;
    std::string summary;      // the latest failing run's
    std::string evidence;     // likewise
    int64_t     opened_at = 0;
    int64_t     last_seen_at = 0;
    int64_t     resolved_at = 0;   // 0 while open
    int64_t     occurrences = 0;   // failing runs while open
    bool        noticed_open = false;
    bool        noticed_resolved = false;
};

enum class Transition { None, Opened, Resolved };

// Evidence is capped before it is stored: it is a log excerpt for a person,
// not a log.
constexpr size_t kMaxEvidenceBytes = 600;

class IncidentStore {
public:
    explicit IncidentStore(const std::string& db_path);
    ~IncidentStore();
    IncidentStore(const IncidentStore&) = delete;
    IncidentStore& operator=(const IncidentStore&) = delete;

    // Records one result. A failing check opens an incident only after
    // `fail_after` failures in a row (a flaky check is not an incident), and
    // a passing one resolves it at once.
    Transition observe(const CheckResult& r, int fail_after, int64_t now);

    // Resolves every open incident whose key starts with `prefix` and is not
    // in `seen`: a probe that ran and no longer reports a check (a cron job
    // deleted, a service removed) must not leave its incident open forever.
    int resolve_unreported(const std::string& prefix, const std::vector<std::string>& seen,
                           int64_t now);

    std::vector<Incident> open_incidents();
    // Newest first, open or not, opened at or after `since`.
    std::vector<Incident> recent(int64_t since, int limit = 50);
    std::optional<Incident> get(int64_t id);

    // Incidents with something nobody has been told yet, oldest first.
    std::vector<Incident> pending_notices();
    // After a notice went out. `resolved` also marks the opening as told
    // (one message said both).
    void mark_noticed(int64_t id, bool resolved);

private:
    sqlite3*   db_ = nullptr;
    std::mutex mu_;
};

} // namespace funes::incidents
