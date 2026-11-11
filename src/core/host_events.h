// =============================================================================
// src/core/host_events.h — the machine's event ring (OS plan, phase 4)
// =============================================================================
// journald already remembers every leaf on every tree; on yoda it holds ~50k
// lines a day from one unit stuck in a restart loop and ~30k access-log lines
// from another. What the box lacks is the other half — a memory that
// abstracts. This table is the step between the two: structured events, built
// from fields rather than free text, with repeats folded into one row and a
// retention window. Only changes and anomalies go further, into the machine's
// memory pool (host_collector.h); samples and routine lifecycle stay here.
//
// Its own connection on memory.db, like UserStore: nothing here is recall.
// =============================================================================

#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace funes::host {

struct Event {
    int64_t     id      = 0;
    int64_t     ts      = 0;   // first seen, epoch seconds
    int64_t     ts_last = 0;   // last seen — repeats fold into one row
    std::string source;        // journal, dpkg, psi, disk, temp, gpu, oom, boot,
                               // llama, file, funes, run
    std::string kind;          // change | failure | anomaly | sample | info
    std::string subject;       // a unit, package, file, metric or program
    std::string detail;        // short; log text only ever quoted and capped
    double      value   = 0;   // samples and runs
    int64_t     count   = 1;
    int64_t     memory_id = 0; // the machine memory it was promoted to, if any
    bool        tainted = false;  // detail quotes text a process wrote
};

class EventStore {
public:
    explicit EventStore(const std::string& db_path);
    ~EventStore();
    EventStore(const EventStore&) = delete;
    EventStore& operator=(const EventStore&) = delete;

    // Stores `e`, or folds it into the row with the same source, kind,
    // subject and detail when that row was last seen under `fold_gap` seconds
    // ago and first seen under `fold_span` ago. The span is what lets a
    // failure that never stops come back as a new row (and a new memory)
    // once a day instead of growing one row forever. Returns the row's id;
    // `folded` says which happened.
    int64_t add(const Event& e, int64_t fold_gap, int64_t fold_span, bool* folded = nullptr);

    void set_memory(int64_t event_id, int64_t memory_id);

    // Events of the given kinds since `since`, oldest first, the newest
    // `limit` of them. `subject_like` (a substring, case-insensitive) narrows.
    std::vector<Event> query(int64_t since, const std::vector<std::string>& kinds,
                             const std::string& subject_like = "", int limit = 200);

    // The values of one series, newest first — what a baseline is computed from.
    std::vector<double> values(const std::string& source, const std::string& subject,
                               int64_t since, int limit = 200);

    // Promoted rows since `since` — the daily ceiling.
    int64_t promoted_since(int64_t since);

    // Collector state that must survive a restart: the journal cursor, the
    // dpkg.log offset, last-seen kernel, boot id, file fingerprints.
    std::optional<std::string> state(const std::string& key);
    void set_state(const std::string& key, const std::string& value);

    // Samples and info older than `samples_before`, everything else older
    // than `events_before`. Returns rows removed.
    int prune(int64_t samples_before, int64_t events_before);

private:
    sqlite3*   db_ = nullptr;
    std::mutex mu_;
};

} // namespace funes::host
