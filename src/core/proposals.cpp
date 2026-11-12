// src/core/proposals.cpp — see proposals.h
#include "proposals.h"
#include "text_utils.h"
#include "sqlite3.h"
#include <ctime>
#include <stdexcept>

namespace funes {

namespace {

// A local copy of the helper users.cpp and memory.cpp each keep: two stores
// on one file that otherwise share nothing.
struct Stmt {
    sqlite3_stmt* p = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite prepare failed: ") +
                                     sqlite3_errmsg(db) + " — SQL: " + sql);
    }
    ~Stmt() { sqlite3_finalize(p); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    void bind_text(int i, const std::string& s) {
        sqlite3_bind_text(p, i, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
    }
    void bind_int64(int i, int64_t v) { sqlite3_bind_int64(p, i, v); }
    bool step() {
        int rc = sqlite3_step(p);
        if (rc == SQLITE_ROW)  return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("sqlite step failed: rc=") + std::to_string(rc));
    }
    std::string col_text(int i) {
        const unsigned char* t = sqlite3_column_text(p, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    int64_t col_int64(int i) { return sqlite3_column_int64(p, i); }
};

void exec_or_throw(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown sqlite error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + msg + " — SQL: " + sql);
    }
}

const char kColumns[] =
    "id, user_id, agent, tool, args, session, origin, effect, status, result, created_at, decided_at, stage";

Proposal read_row(Stmt& s) {
    Proposal p;
    p.id         = s.col_int64(0);
    p.user_id    = s.col_int64(1);
    p.agent      = s.col_text(2);
    p.tool       = s.col_text(3);
    p.args       = nlohmann::json::parse(s.col_text(4), nullptr, false);
    if (p.args.is_discarded()) p.args = nlohmann::json::object();
    p.session    = s.col_text(5);
    p.origin     = s.col_text(6);
    p.effect     = s.col_text(7);
    p.status     = s.col_text(8);
    p.result     = s.col_text(9);
    p.created_at = s.col_int64(10);
    p.decided_at = s.col_int64(11);
    p.stage      = s.col_text(12);
    return p;
}

} // namespace

ProposalStore::ProposalStore(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw std::runtime_error("Cannot open proposal database '" + db_path + "': " + msg);
    }
    exec_or_throw(db_, "PRAGMA journal_mode=WAL;");
    exec_or_throw(db_, "PRAGMA busy_timeout=5000;");
    exec_or_throw(db_, R"sql(
        CREATE TABLE IF NOT EXISTS proposals (
            id          INTEGER PRIMARY KEY,
            user_id     INTEGER NOT NULL,
            agent       TEXT NOT NULL,
            tool        TEXT NOT NULL,
            args        TEXT NOT NULL,
            session     TEXT NOT NULL DEFAULT '',
            origin      TEXT NOT NULL DEFAULT '',
            effect      TEXT NOT NULL DEFAULT 'external',
            status      TEXT NOT NULL DEFAULT 'pending',
            result      TEXT NOT NULL DEFAULT '',
            created_at  INTEGER NOT NULL,
            decided_at  INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_proposals_user ON proposals(user_id, status);
    )sql");
    // 7.12: a staged call's stage directory. 7.11 databases lack the column.
    bool has_stage = false;
    {
        Stmt s(db_, "PRAGMA table_info(proposals)");
        while (s.step()) if (s.col_text(1) == "stage") has_stage = true;
    }
    if (!has_stage) exec_or_throw(db_, "ALTER TABLE proposals ADD COLUMN stage TEXT NOT NULL DEFAULT ''");
}

ProposalStore::~ProposalStore() {
    if (db_) sqlite3_close(db_);
}

int64_t ProposalStore::now() const {
    return now_override_ ? now_override_ : static_cast<int64_t>(std::time(nullptr));
}

int64_t ProposalStore::create(int64_t user_id, const std::string& agent, const std::string& tool,
                              const nlohmann::json& args, const std::string& session,
                              const std::string& origin, const std::string& effect,
                              const std::string& stage) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "INSERT INTO proposals (user_id, agent, tool, args, session, origin, effect, created_at, stage) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    s.bind_int64(1, user_id);
    s.bind_text(2, agent);
    s.bind_text(3, tool);
    s.bind_text(4, funes::dump_safe(args));
    s.bind_text(5, session);
    s.bind_text(6, origin);
    s.bind_text(7, effect);
    s.bind_int64(8, now());
    s.bind_text(9, stage);
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

std::optional<Proposal> ProposalStore::get(int64_t user_id, int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, (std::string("SELECT ") + kColumns + " FROM proposals WHERE id = ? AND user_id = ?").c_str());
    s.bind_int64(1, id);
    s.bind_int64(2, user_id);
    if (!s.step()) return std::nullopt;
    return read_row(s);
}

ProposalStore::Decision ProposalStore::decide(int64_t user_id, int64_t id, bool approve) {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t t = now();
    {
        Stmt s(db_, "UPDATE proposals SET status = ?, decided_at = ? "
                    "WHERE id = ? AND user_id = ? AND status = 'pending' AND created_at > ?");
        s.bind_text(1, approve ? "approved" : "rejected");
        s.bind_int64(2, t);
        s.bind_int64(3, id);
        s.bind_int64(4, user_id);
        s.bind_int64(5, t - kTtlSeconds);
        s.step();
        if (sqlite3_changes(db_) == 1) return Decision::Ok;
    }
    // Why not: one more read, only on the refusal path.
    Stmt s(db_, "SELECT status, created_at FROM proposals WHERE id = ? AND user_id = ?");
    s.bind_int64(1, id);
    s.bind_int64(2, user_id);
    if (!s.step()) return Decision::NotFound;
    if (s.col_text(0) != "pending") return Decision::AlreadyDecided;
    return Decision::Expired;
}

void ProposalStore::finish(int64_t user_id, int64_t id, bool ok, const std::string& result) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string capped = result.size() > 4000 ? result.substr(0, 4000) : result;
    Stmt s(db_, "UPDATE proposals SET status = ?, result = ? "
                "WHERE id = ? AND user_id = ? AND status = 'approved'");
    s.bind_text(1, ok ? "done" : "failed");
    s.bind_text(2, capped);
    s.bind_int64(3, id);
    s.bind_int64(4, user_id);
    s.step();
}

std::vector<Proposal> ProposalStore::pending(int64_t user_id, int limit) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, (std::string("SELECT ") + kColumns +
                 " FROM proposals WHERE user_id = ? AND status = 'pending' AND created_at > ?"
                 " ORDER BY id DESC LIMIT ?").c_str());
    s.bind_int64(1, user_id);
    s.bind_int64(2, now() - kTtlSeconds);
    s.bind_int64(3, limit);
    std::vector<Proposal> out;
    while (s.step()) out.push_back(read_row(s));
    return out;
}

std::set<std::string> ProposalStore::live_stages() {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT stage FROM proposals WHERE stage != '' AND status IN ('pending', 'approved') "
                "AND created_at > ?");
    s.bind_int64(1, now() - kTtlSeconds);
    std::set<std::string> out;
    while (s.step()) out.insert(s.col_text(0));
    return out;
}

} // namespace funes
