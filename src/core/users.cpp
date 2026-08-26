// =============================================================================
// src/core/users.cpp — accounts, auth tokens, WhatsApp identity (implementation)
// =============================================================================

#include "users.h"
#include "password.h"
#include "sqlite3.h"
#include <iostream>
#include <stdexcept>

// ── small sqlite helpers ──────────────────────────────────────────────────────
// Deliberately a local copy of memory.cpp's helpers rather than a shared
// header: they are 30 lines, and hoisting them into a common utility would
// couple two stores that otherwise share nothing but a file path.

namespace {

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

// The seven columns of `users`, in the order read_user() expects. The password
// hash is deliberately absent: it is only ever selected by the one query that
// needs it (verify_login), so it cannot leak into a User handed to the API.
const char USER_COLUMNS[] =
    "id, username, display_name, role, permissions, created_at";

UserStore::User read_user(Stmt& s) {
    UserStore::User u;
    u.id           = s.col_int64(0);
    u.username     = s.col_text(1);
    u.display_name = s.col_text(2);
    u.role         = s.col_text(3);
    u.permissions  = s.col_text(4);
    u.created_at   = s.col_text(5);
    return u;
}

} // namespace

// ── construction / schema ─────────────────────────────────────────────────────

UserStore::UserStore(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw std::runtime_error("Cannot open user database '" + db_path + "': " + msg);
    }

    exec_or_throw(db_, "PRAGMA journal_mode=WAL;");
    exec_or_throw(db_, "PRAGMA busy_timeout=5000;");
    exec_or_throw(db_, "PRAGMA foreign_keys=ON;");
    migrate();
}

UserStore::~UserStore() {
    if (db_) sqlite3_close(db_);
}

void UserStore::migrate() {
    // ON DELETE CASCADE on both child tables is what makes delete_user() safe:
    // a token or jid mapping surviving its user would authenticate a request
    // as a row that no longer exists. foreign_keys=ON above is required for
    // this to be enforced — SQLite ignores it otherwise.
    exec_or_throw(db_, R"sql(
        CREATE TABLE IF NOT EXISTS users (
            id            INTEGER PRIMARY KEY,
            username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
            display_name  TEXT NOT NULL DEFAULT '',
            password_hash TEXT NOT NULL,
            role          TEXT NOT NULL DEFAULT 'member',
            permissions   TEXT NOT NULL DEFAULT '{}',
            created_at    TEXT NOT NULL DEFAULT (datetime('now'))
        );
        CREATE TABLE IF NOT EXISTS auth_tokens (
            token      TEXT PRIMARY KEY,
            user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            expires_at TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_auth_tokens_user ON auth_tokens(user_id);
        CREATE TABLE IF NOT EXISTS jid_users (
            chat_jid TEXT PRIMARY KEY,
            user_id  INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE
        );
    )sql");
}

// ── accounts ──────────────────────────────────────────────────────────────────

int64_t UserStore::create_user(const std::string& username, const std::string& password,
                               const std::string& display_name, const std::string& role) {
    if (username.empty() || password.empty()) return 0;
    if (role != ROLE_ADMIN && role != ROLE_MEMBER) return 0;

    // Hash outside the lock: PBKDF2 is ~200ms by design, and holding the
    // store's mutex for it would serialise every other caller behind a login.
    const std::string hash = funes::hash_password(password);

    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "INSERT OR IGNORE INTO users(username, password_hash, display_name, role) "
                "VALUES(?, ?, ?, ?)");
    s.bind_text(1, username);
    s.bind_text(2, hash);
    s.bind_text(3, display_name);
    s.bind_text(4, role);
    s.step();
    // OR IGNORE turns the UNIQUE violation into zero changed rows, which is
    // the "username taken" signal the header documents.
    if (sqlite3_changes(db_) == 0) return 0;
    return sqlite3_last_insert_rowid(db_);
}

std::optional<UserStore::User> UserStore::find_by_username(const std::string& username) {
    if (username.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, (std::string("SELECT ") + USER_COLUMNS +
                 " FROM users WHERE username = ?").c_str());
    s.bind_text(1, username);
    if (!s.step()) return std::nullopt;
    return read_user(s);
}

std::optional<UserStore::User> UserStore::find_by_id(int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, (std::string("SELECT ") + USER_COLUMNS +
                 " FROM users WHERE id = ?").c_str());
    s.bind_int64(1, id);
    if (!s.step()) return std::nullopt;
    return read_user(s);
}

std::optional<UserStore::User> UserStore::verify_login(const std::string& username,
                                                       const std::string& password) {
    if (username.empty() || password.empty()) return std::nullopt;

    std::string hash;
    {
        std::lock_guard<std::mutex> lock(mu_);
        Stmt s(db_, "SELECT password_hash FROM users WHERE username = ?");
        s.bind_text(1, username);
        if (!s.step()) return std::nullopt;
        hash = s.col_text(0);
    }
    // Verify outside the lock, for the same reason create_user hashes outside
    // it: a wrong password must not be able to stall every other request.
    if (!funes::verify_password(password, hash)) return std::nullopt;
    return find_by_username(username);
}

bool UserStore::set_password(int64_t user_id, const std::string& password) {
    if (password.empty()) return false;
    const std::string hash = funes::hash_password(password);

    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "UPDATE users SET password_hash = ? WHERE id = ?");
    s.bind_text(1, hash);
    s.bind_int64(2, user_id);
    s.step();
    return sqlite3_changes(db_) > 0;
}

bool UserStore::set_permissions(int64_t user_id, const std::string& permissions_json) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "UPDATE users SET permissions = ? WHERE id = ?");
    s.bind_text(1, permissions_json);
    s.bind_int64(2, user_id);
    s.step();
    return sqlite3_changes(db_) > 0;
}

bool UserStore::delete_user(int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    // auth_tokens and jid_users cascade (see migrate()).
    Stmt s(db_, "DELETE FROM users WHERE id = ?");
    s.bind_int64(1, id);
    s.step();
    return sqlite3_changes(db_) > 0;
}

std::vector<UserStore::User> UserStore::list_users() {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, (std::string("SELECT ") + USER_COLUMNS +
                 " FROM users ORDER BY id").c_str());
    std::vector<User> out;
    while (s.step()) out.push_back(read_user(s));
    return out;
}

int64_t UserStore::count() {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT COUNT(*) FROM users");
    return s.step() ? s.col_int64(0) : 0;
}

// ── tokens ────────────────────────────────────────────────────────────────────

std::string UserStore::create_token(int64_t user_id, int ttl_days) {
    const std::string token = funes::random_token(32);

    std::lock_guard<std::mutex> lock(mu_);
    // Expiry is computed by SQLite rather than in C++ so it is written in the
    // same clock and format resolve_token() compares against.
    Stmt s(db_, "INSERT INTO auth_tokens(token, user_id, expires_at) "
                "VALUES(?, ?, datetime('now', ?))");
    s.bind_text(1, token);
    s.bind_int64(2, user_id);
    s.bind_text(3, std::to_string(ttl_days) + " days");
    s.step();
    if (sqlite3_changes(db_) == 0) return "";
    return token;
}

std::optional<UserStore::User> UserStore::resolve_token(const std::string& token) {
    if (token.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mu_);
    // The expiry predicate lives in the query, not in a later branch: an
    // expired token must read as "no such token" on the one code path every
    // authenticated request uses, whether or not the purge has ever run.
    // The join is what makes a token whose user was deleted resolve to
    // nothing even if foreign keys were somehow off.
    Stmt s(db_, (std::string("SELECT u.") + "id, u.username, u.display_name, u.role, "
                 "u.permissions, u.created_at "
                 "FROM auth_tokens t JOIN users u ON u.id = t.user_id "
                 "WHERE t.token = ? AND t.expires_at > datetime('now')").c_str());
    s.bind_text(1, token);
    if (!s.step()) return std::nullopt;
    return read_user(s);
}

bool UserStore::revoke_token(const std::string& token) {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "DELETE FROM auth_tokens WHERE token = ?");
    s.bind_text(1, token);
    s.step();
    return sqlite3_changes(db_) > 0;
}

int UserStore::purge_expired_tokens() {
    std::lock_guard<std::mutex> lock(mu_);
    exec_or_throw(db_, "DELETE FROM auth_tokens WHERE expires_at <= datetime('now');");
    return sqlite3_changes(db_);
}

// ── WhatsApp identity ─────────────────────────────────────────────────────────

bool UserStore::map_jid(const std::string& chat_jid, int64_t user_id) {
    if (chat_jid.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    // REPLACE so remapping a number to a different person moves the row
    // instead of failing or leaving two. The foreign key is what refuses a
    // mapping to a user that doesn't exist.
    try {
        Stmt s(db_, "INSERT OR REPLACE INTO jid_users(chat_jid, user_id) VALUES(?, ?)");
        s.bind_text(1, chat_jid);
        s.bind_int64(2, user_id);
        s.step();
        return sqlite3_changes(db_) > 0;
    } catch (const std::exception&) {
        return false;  // FOREIGN KEY constraint failed — unknown user_id
    }
}

std::optional<UserStore::User> UserStore::resolve_jid(const std::string& chat_jid) {
    if (chat_jid.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT u.id, u.username, u.display_name, u.role, u.permissions, "
                "u.created_at FROM jid_users j JOIN users u ON u.id = j.user_id "
                "WHERE j.chat_jid = ?");
    s.bind_text(1, chat_jid);
    if (!s.step()) return std::nullopt;
    return read_user(s);
}

bool UserStore::unmap_jid(const std::string& chat_jid) {
    if (chat_jid.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "DELETE FROM jid_users WHERE chat_jid = ?");
    s.bind_text(1, chat_jid);
    s.step();
    return sqlite3_changes(db_) > 0;
}
