// src/core/host_events.cpp — see host_events.h
#include "host_events.h"
#include "sqlite_util.h"
#include <algorithm>

using funes::sqlite::Stmt;
using funes::sqlite::exec_or_throw;

namespace funes::host {

namespace {

const char EVENT_COLUMNS[] =
    "id, ts, ts_last, source, kind, subject, detail, value, count, memory_id, tainted";

Event read_event(Stmt& s) {
    Event e;
    e.id        = s.col_int64(0);
    e.ts        = s.col_int64(1);
    e.ts_last   = s.col_int64(2);
    e.source    = s.col_text(3);
    e.kind      = s.col_text(4);
    e.subject   = s.col_text(5);
    e.detail    = s.col_text(6);
    e.value     = s.col_double(7);
    e.count     = s.col_int64(8);
    e.memory_id = s.col_int64(9);
    e.tainted   = s.col_int64(10) != 0;
    return e;
}

} // namespace

EventStore::EventStore(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw std::runtime_error("Cannot open host event store '" + db_path + "': " + msg);
    }
    exec_or_throw(db_, "PRAGMA journal_mode=WAL;");
    exec_or_throw(db_, "PRAGMA busy_timeout=5000;");
    exec_or_throw(db_, R"sql(
        CREATE TABLE IF NOT EXISTS host_events (
            id        INTEGER PRIMARY KEY,
            ts        INTEGER NOT NULL,
            ts_last   INTEGER NOT NULL,
            source    TEXT NOT NULL,
            kind      TEXT NOT NULL,
            subject   TEXT NOT NULL,
            detail    TEXT NOT NULL DEFAULT '',
            value     REAL NOT NULL DEFAULT 0,
            count     INTEGER NOT NULL DEFAULT 1,
            memory_id INTEGER NOT NULL DEFAULT 0,
            tainted   INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_host_events_ts   ON host_events(ts_last);
        CREATE INDEX IF NOT EXISTS idx_host_events_key  ON host_events(source, kind, subject, ts_last);
        CREATE TABLE IF NOT EXISTS host_state (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )sql");
}

EventStore::~EventStore() {
    if (db_) sqlite3_close(db_);
}

int64_t EventStore::add(const Event& e, int64_t fold_gap, int64_t fold_span, bool* folded) {
    std::lock_guard<std::mutex> lock(mu_);
    if (fold_gap > 0) {
        Stmt s(db_, "SELECT id FROM host_events WHERE source = ? AND kind = ? AND subject = ? "
                    "AND detail = ? AND ts_last >= ? AND ts >= ? ORDER BY id DESC LIMIT 1");
        s.bind_text(1, e.source);
        s.bind_text(2, e.kind);
        s.bind_text(3, e.subject);
        s.bind_text(4, e.detail);
        s.bind_int64(5, e.ts - fold_gap);
        s.bind_int64(6, e.ts - fold_span);
        if (s.step()) {
            const int64_t id = s.col_int64(0);
            Stmt u(db_, "UPDATE host_events SET ts_last = MAX(ts_last, ?), count = count + ?, "
                        "value = ? WHERE id = ?");
            u.bind_int64(1, e.ts);
            u.bind_int64(2, std::max<int64_t>(1, e.count));
            u.bind_double(3, e.value);
            u.bind_int64(4, id);
            u.step();
            if (folded) *folded = true;
            return id;
        }
    }
    Stmt s(db_, "INSERT INTO host_events(ts, ts_last, source, kind, subject, detail, value, "
                "count, tainted) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)");
    s.bind_int64(1, e.ts);
    s.bind_int64(2, e.ts_last > e.ts ? e.ts_last : e.ts);
    s.bind_text(3, e.source);
    s.bind_text(4, e.kind);
    s.bind_text(5, e.subject);
    s.bind_text(6, e.detail);
    s.bind_double(7, e.value);
    s.bind_int64(8, std::max<int64_t>(1, e.count));
    s.bind_int64(9, e.tainted ? 1 : 0);
    s.step();
    if (folded) *folded = false;
    return sqlite3_last_insert_rowid(db_);
}

void EventStore::set_memory(int64_t event_id, int64_t memory_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "UPDATE host_events SET memory_id = ? WHERE id = ?");
    s.bind_int64(1, memory_id);
    s.bind_int64(2, event_id);
    s.step();
}

std::vector<Event> EventStore::query(int64_t since, const std::vector<std::string>& kinds,
                                     const std::string& subject_like, int limit) {
    std::string sql = std::string("SELECT ") + EVENT_COLUMNS +
                      " FROM host_events WHERE ts_last >= ?";
    if (!kinds.empty()) {
        sql += " AND kind IN (";
        for (size_t i = 0; i < kinds.size(); ++i) sql += i ? ",?" : "?";
        sql += ")";
    }
    if (!subject_like.empty()) sql += " AND instr(lower(subject), lower(?)) > 0";
    sql += " ORDER BY ts DESC, id DESC LIMIT ?";

    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, sql.c_str());
    int i = 1;
    s.bind_int64(i++, since);
    for (const auto& k : kinds) s.bind_text(i++, k);
    if (!subject_like.empty()) s.bind_text(i++, subject_like);
    s.bind_int64(i, std::max(1, limit));
    std::vector<Event> out;
    while (s.step()) out.push_back(read_event(s));
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<double> EventStore::values(const std::string& source, const std::string& subject,
                                       int64_t since, int limit) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT value FROM host_events WHERE source = ? AND subject = ? "
                "AND kind IN ('sample', 'info') AND ts >= ? ORDER BY ts DESC LIMIT ?");
    s.bind_text(1, source);
    s.bind_text(2, subject);
    s.bind_int64(3, since);
    s.bind_int64(4, limit);
    std::vector<double> out;
    while (s.step()) out.push_back(s.col_double(0));
    return out;
}

int64_t EventStore::promoted_since(int64_t since) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT COUNT(*) FROM host_events WHERE memory_id > 0 AND ts >= ?");
    s.bind_int64(1, since);
    return s.step() ? s.col_int64(0) : 0;
}

std::optional<std::string> EventStore::state(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT value FROM host_state WHERE key = ?");
    s.bind_text(1, key);
    if (!s.step()) return std::nullopt;
    return s.col_text(0);
}

void EventStore::set_state(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "INSERT INTO host_state(key, value) VALUES(?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind_text(1, key);
    s.bind_text(2, value);
    s.step();
}

int EventStore::prune(int64_t samples_before, int64_t events_before) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt a(db_, "DELETE FROM host_events WHERE kind IN ('sample', 'info') AND ts_last < ?");
    a.bind_int64(1, samples_before);
    a.step();
    int n = sqlite3_changes(db_);
    Stmt b(db_, "DELETE FROM host_events WHERE ts_last < ?");
    b.bind_int64(1, events_before);
    b.step();
    return n + sqlite3_changes(db_);
}

} // namespace funes::host
