// =============================================================================
// src/core/memory_facts.cpp — the fact extraction queue (memory plan, phase 4)
// =============================================================================
// MemoryStore members, in their own file because memory.cpp is already the
// largest in the core. See memory.h, "Fact extraction queue".

#include "memory.h"
#include "sqlite_util.h"
#include <iostream>
#include <mutex>

using funes::sqlite::Stmt;
using funes::sqlite::exec_or_throw;

void MemoryStore::migrate_facts() {
    // Ownership is the row's user_id, like every other table here: the pass
    // stores each fact under the user and agent the exchange was queued with,
    // and never looks anything up by exchange id alone.
    exec_or_throw(db_, R"sql(
        CREATE TABLE IF NOT EXISTS memory_extract_queue (
            id            INTEGER PRIMARY KEY,
            user_id       INTEGER NOT NULL,
            agent         TEXT NOT NULL,
            session       TEXT NOT NULL DEFAULT '',
            user_text     TEXT NOT NULL,
            reply         TEXT NOT NULL,
            origin        TEXT,
            tainted       INTEGER NOT NULL DEFAULT 0,
            source_memory INTEGER NOT NULL DEFAULT 0,
            status        TEXT NOT NULL DEFAULT 'pending',   -- pending | done | failed
            attempts      INTEGER NOT NULL DEFAULT 0,
            facts         INTEGER NOT NULL DEFAULT 0,
            created_at    TEXT NOT NULL DEFAULT (datetime('now')),
            finished_at   TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_extract_queue_pending
            ON memory_extract_queue(status, id);
    )sql");
}

int64_t MemoryStore::enqueue_exchange(int64_t user_id, const std::string& agent,
                                      const std::string& session, const std::string& user_text,
                                      const std::string& reply, const funes::Provenance& prov,
                                      int64_t source_memory) {
    if (user_id <= 0 || agent.empty())
        throw std::runtime_error("enqueue_exchange: a user and an agent are required");
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "INSERT INTO memory_extract_queue"
                "(user_id, agent, session, user_text, reply, origin, tainted, source_memory) "
                "VALUES(?,?,?,?,?,?,?,?)");
    s.bind_int64(1, user_id);
    s.bind_text(2, agent);
    s.bind_text(3, session);
    s.bind_text(4, user_text);
    s.bind_text(5, reply);
    if (prov.origin.empty()) sqlite3_bind_null(s.p, 6);
    else                     s.bind_text(6, prov.origin);
    s.bind_int64(7, prov.tainted ? 1 : 0);
    s.bind_int64(8, source_memory);
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

std::vector<MemoryStore::PendingExchange> MemoryStore::pending_exchanges(size_t limit,
                                                                        int min_age_seconds) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT id, user_id, agent, session, user_text, reply, origin, tainted, "
                "source_memory, attempts, created_at FROM memory_extract_queue "
                "WHERE status = 'pending' "
                "AND created_at <= datetime('now', '-' || ? || ' seconds') "
                "ORDER BY id LIMIT ?");
    s.bind_int64(1, min_age_seconds);
    s.bind_int64(2, static_cast<int64_t>(limit));
    std::vector<PendingExchange> out;
    while (s.step()) {
        PendingExchange e;
        e.id            = s.col_int64(0);
        e.user_id       = s.col_int64(1);
        e.agent         = s.col_text(2);
        e.session       = s.col_text(3);
        e.user_text     = s.col_text(4);
        e.reply         = s.col_text(5);
        e.origin        = s.col_text(6);
        e.tainted       = s.col_int64(7) != 0;
        e.source_memory = s.col_int64(8);
        e.attempts      = static_cast<int>(s.col_int64(9));
        e.created_at    = s.col_text(10);
        out.push_back(std::move(e));
    }
    return out;
}

void MemoryStore::finish_exchange(int64_t id, bool ok, int facts) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, ok
        ? "UPDATE memory_extract_queue SET status = 'done', facts = ?2, "
          "attempts = attempts + 1, finished_at = datetime('now') WHERE id = ?1"
        : "UPDATE memory_extract_queue SET attempts = attempts + 1, "
          "status = CASE WHEN attempts + 1 >= ?2 THEN 'failed' ELSE 'pending' END, "
          "finished_at = datetime('now') WHERE id = ?1");
    s.bind_int64(1, id);
    s.bind_int64(2, ok ? facts : kMaxExtractAttempts);
    s.step();
}

int64_t MemoryStore::count_pending_exchanges() {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT COUNT(*) FROM memory_extract_queue WHERE status = 'pending'");
    return s.step() ? s.col_int64(0) : 0;
}

// ── supersession (phase 5) ───────────────────────────────────────────────────

bool MemoryStore::supersede(int64_t user_id, int64_t older, int64_t newer) {
    if (user_id <= 0 || older <= 0 || newer <= 0 || older == newer) return false;
    std::lock_guard<std::mutex> lock(mu_);
    // Every guard is in the statement, like every ownership check in this
    // store: the UPDATE either matches a row that satisfies all of them or
    // changes nothing, so there is no read-then-write window to race.
    Stmt s(db_, R"sql(
        UPDATE memories SET superseded_by = ?3, superseded_at = datetime('now')
        WHERE id = ?2 AND user_id = ?1 AND superseded_by IS NULL
          AND EXISTS (
              SELECT 1 FROM memories n
              WHERE n.id = ?3 AND n.user_id = ?1 AND n.superseded_by IS NULL
                AND (n.tainted = 0 OR memories.tainted = 1)
                AND (memories.source <> 'user' OR n.source = 'user'))
    )sql");
    s.bind_int64(1, user_id);
    s.bind_int64(2, older);
    s.bind_int64(3, newer);
    s.step();
    const bool changed = sqlite3_changes(db_) > 0;
    if (changed)
        std::cerr << "[memory] #" << older << " superseded by #" << newer << "\n";
    return changed;
}

bool MemoryStore::unsupersede(int64_t user_id, int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "UPDATE memories SET superseded_by = NULL, superseded_at = NULL "
                "WHERE id = ? AND user_id = ? AND superseded_by IS NOT NULL");
    s.bind_int64(1, id);
    s.bind_int64(2, user_id);
    s.step();
    return sqlite3_changes(db_) > 0;
}

std::pair<int64_t, double> MemoryStore::nearest_live(int64_t user_id, const std::string& agent,
                                                     const std::string& text) {
    std::vector<float> vec;
    if (!try_embed(text, vec)) return {0, 0.0};
    std::lock_guard<std::mutex> lock(mu_);
    if (dim_ == 0 || dim_ != static_cast<int>(vec.size())) return {0, 0.0};
    Stmt s(db_, R"sql(
        SELECT m.id, v.distance, m.agent, m.superseded_by IS NULL AND m.archived = 0
        FROM vec_memories v
        JOIN memories m ON m.id = v.memory_id
        WHERE v.user_id = ? AND v.embedding MATCH ? AND v.k = 16
        ORDER BY v.distance
    )sql");
    s.bind_int64(1, user_id);
    s.bind_blob(2, vec.data(), vec.size() * sizeof(float));
    while (s.step()) {
        if (s.col_text(2) != agent || s.col_int64(3) == 0) continue;
        return {s.col_int64(0), 1.0 - s.col_double(1)};
    }
    return {0, 0.0};
}
