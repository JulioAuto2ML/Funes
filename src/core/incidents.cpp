// src/core/incidents.cpp — see incidents.h
#include "incidents.h"
#include "sqlite_util.h"
#include "text_utils.h"
#include <algorithm>
#include <stdexcept>

using funes::sqlite::Stmt;
using funes::sqlite::exec_or_throw;

namespace funes::incidents {

namespace {

const char COLUMNS[] =
    "id, key, severity, summary, evidence, opened_at, last_seen_at, resolved_at, "
    "occurrences, noticed_open, noticed_resolved";

Incident read_incident(Stmt& s) {
    Incident i;
    i.id               = s.col_int64(0);
    i.key              = s.col_text(1);
    i.severity         = s.col_text(2);
    i.summary          = s.col_text(3);
    i.evidence         = s.col_text(4);
    i.opened_at        = s.col_int64(5);
    i.last_seen_at     = s.col_int64(6);
    i.resolved_at      = s.col_int64(7);
    i.occurrences      = s.col_int64(8);
    i.noticed_open     = s.col_int64(9) != 0;
    i.noticed_resolved = s.col_int64(10) != 0;
    return i;
}

std::string capped(std::string s) {
    if (s.size() > kMaxEvidenceBytes) {
        truncate_utf8_safe(s, kMaxEvidenceBytes);
        s += "…";
    }
    return s;
}

std::vector<Incident> query(sqlite3* db, const std::string& where_order, int64_t arg = 0,
                            bool bind = false, int limit = 0) {
    std::string sql = std::string("SELECT ") + COLUMNS + " FROM incidents " + where_order;
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    Stmt s(db, sql.c_str());
    if (bind) s.bind_int64(1, arg);
    std::vector<Incident> out;
    while (s.step()) out.push_back(read_incident(s));
    return out;
}

} // namespace

IncidentStore::IncidentStore(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw std::runtime_error("Cannot open incident store '" + db_path + "': " + msg);
    }
    exec_or_throw(db_, "PRAGMA journal_mode=WAL;");
    exec_or_throw(db_, "PRAGMA busy_timeout=5000;");
    exec_or_throw(db_, R"sql(
        CREATE TABLE IF NOT EXISTS incidents (
            id               INTEGER PRIMARY KEY,
            key              TEXT NOT NULL,
            severity         TEXT NOT NULL DEFAULT 'warning',
            summary          TEXT NOT NULL DEFAULT '',
            evidence         TEXT NOT NULL DEFAULT '',
            opened_at        INTEGER NOT NULL,
            last_seen_at     INTEGER NOT NULL,
            resolved_at      INTEGER NOT NULL DEFAULT 0,
            occurrences      INTEGER NOT NULL DEFAULT 1,
            noticed_open     INTEGER NOT NULL DEFAULT 0,
            noticed_resolved INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_incidents_open ON incidents(resolved_at, key);
        -- Consecutive failures per check, for fail_after. One row per key.
        CREATE TABLE IF NOT EXISTS incident_checks (
            key          TEXT PRIMARY KEY,
            failures     INTEGER NOT NULL DEFAULT 0,
            last_run_at  INTEGER NOT NULL DEFAULT 0
        );
    )sql");
}

IncidentStore::~IncidentStore() {
    if (db_) sqlite3_close(db_);
}

Transition IncidentStore::observe(const CheckResult& r, int fail_after, int64_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    int64_t failures = 0;
    {
        Stmt s(db_, R"sql(
            INSERT INTO incident_checks(key, failures, last_run_at) VALUES(?, ?, ?)
            ON CONFLICT(key) DO UPDATE SET
                failures = CASE WHEN excluded.failures = 0 THEN 0 ELSE failures + 1 END,
                last_run_at = excluded.last_run_at
            RETURNING failures
        )sql");
        s.bind_text(1, r.key);
        s.bind_int64(2, r.ok ? 0 : 1);
        s.bind_int64(3, now);
        if (s.step()) failures = s.col_int64(0);
    }

    int64_t open_id = 0;
    {
        Stmt s(db_, "SELECT id FROM incidents WHERE key = ? AND resolved_at = 0 "
                    "ORDER BY id DESC LIMIT 1");
        s.bind_text(1, r.key);
        if (s.step()) open_id = s.col_int64(0);
    }

    if (r.ok) {
        if (!open_id) return Transition::None;
        Stmt s(db_, "UPDATE incidents SET resolved_at = ? WHERE id = ?");
        s.bind_int64(1, now);
        s.bind_int64(2, open_id);
        s.step();
        return Transition::Resolved;
    }

    const std::string severity = r.severity.empty() ? "warning" : r.severity;
    if (open_id) {
        Stmt s(db_, "UPDATE incidents SET summary = ?, evidence = ?, last_seen_at = ?, "
                    "occurrences = occurrences + 1, severity = ? WHERE id = ?");
        s.bind_text(1, r.summary);
        s.bind_text(2, capped(r.evidence));
        s.bind_int64(3, now);
        s.bind_text(4, severity);
        s.bind_int64(5, open_id);
        s.step();
        return Transition::None;
    }
    if (failures < std::max(1, fail_after)) return Transition::None;

    // Opened once fail_after is reached; the failures before it count too.
    Stmt s(db_, "INSERT INTO incidents(key, severity, summary, evidence, opened_at, "
                "last_seen_at, occurrences) VALUES(?, ?, ?, ?, ?, ?, ?)");
    s.bind_text(1, r.key);
    s.bind_text(2, severity);
    s.bind_text(3, r.summary);
    s.bind_text(4, capped(r.evidence));
    s.bind_int64(5, now);
    s.bind_int64(6, now);
    s.bind_int64(7, failures);
    s.step();
    return Transition::Opened;
}

int IncidentStore::resolve_unreported(const std::string& prefix,
                                      const std::vector<std::string>& seen, int64_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<int64_t, std::string>> open;
    {
        Stmt s(db_, "SELECT id, key FROM incidents WHERE resolved_at = 0 AND substr(key, 1, ?) = ?");
        s.bind_int64(1, static_cast<int64_t>(prefix.size()));
        s.bind_text(2, prefix);
        while (s.step()) open.emplace_back(s.col_int64(0), s.col_text(1));
    }
    int n = 0;
    for (const auto& [id, key] : open) {
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        Stmt s(db_, "UPDATE incidents SET resolved_at = ? WHERE id = ?");
        s.bind_int64(1, now);
        s.bind_int64(2, id);
        s.step();
        Stmt c(db_, "DELETE FROM incident_checks WHERE key = ?");
        c.bind_text(1, key);
        c.step();
        ++n;
    }
    return n;
}

std::vector<Incident> IncidentStore::open_incidents() {
    std::lock_guard<std::mutex> lock(mu_);
    return query(db_, "WHERE resolved_at = 0 ORDER BY id");
}

std::vector<Incident> IncidentStore::recent(int64_t since, int limit) {
    std::lock_guard<std::mutex> lock(mu_);
    return query(db_, "WHERE opened_at >= ? ORDER BY id DESC", since, true, std::max(1, limit));
}

std::optional<Incident> IncidentStore::get(int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto rows = query(db_, "WHERE id = ?", id, true);
    if (rows.empty()) return std::nullopt;
    return rows[0];
}

std::vector<Incident> IncidentStore::pending_notices() {
    std::lock_guard<std::mutex> lock(mu_);
    return query(db_, "WHERE noticed_open = 0 OR (resolved_at > 0 AND noticed_resolved = 0) "
                      "ORDER BY id");
}

void IncidentStore::mark_noticed(int64_t id, bool resolved) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, resolved
        ? "UPDATE incidents SET noticed_open = 1, noticed_resolved = 1 WHERE id = ?"
        : "UPDATE incidents SET noticed_open = 1 WHERE id = ?");
    s.bind_int64(1, id);
    s.step();
}

} // namespace funes::incidents
