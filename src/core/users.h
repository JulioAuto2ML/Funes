// =============================================================================
// src/core/users.h — accounts, auth tokens, and WhatsApp identity mapping
// =============================================================================
//
// Phase 1 of docs/dev-plan-users-permissions.md. Three tables in the same
// SQLite file MemoryStore uses:
//
//   users       — id, username, display_name, password_hash, role, permissions
//   auth_tokens — opaque cookie value → user, with an expiry
//   jid_users   — WhatsApp chat_jid → user (the number *is* the credential)
//
// Its own class and its own connection rather than more methods on
// MemoryStore: authentication is a different concern from recall, memory.cpp
// is already ~950 lines, and nothing here needs the embedder. Two connections
// to one file is fine — the database is opened WAL with a busy timeout, the
// same as MemoryStore.
//
// No self-registration by design. The admin creates accounts from the CLI
// (`funes useradd`), which keeps the authentication surface to one endpoint.
//
// Thread-safe: one connection guarded by a mutex, matching MemoryStore.
// =============================================================================

#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

class UserStore {
public:
    // Roles. `admin` may manage users and use every agent and tool; `member`
    // is scoped by the per-user permissions blob (phase 4 reads it; phase 1
    // only stores it).
    static constexpr const char* ROLE_ADMIN  = "admin";
    static constexpr const char* ROLE_MEMBER = "member";

    struct User {
        int64_t     id = 0;
        std::string username;
        std::string display_name;
        std::string role;         // ROLE_ADMIN | ROLE_MEMBER
        std::string permissions;  // JSON blob, "{}" until phase 4
        std::string created_at;   // UTC "YYYY-MM-DD HH:MM:SS"

        bool is_admin() const { return role == ROLE_ADMIN; }
    };

    // Throws std::runtime_error if the database cannot be opened or migrated.
    explicit UserStore(const std::string& db_path);
    ~UserStore();

    UserStore(const UserStore&)            = delete;
    UserStore& operator=(const UserStore&) = delete;

    // ── accounts ──────────────────────────────────────────────────────────────

    // Returns the new user's id, or 0 if the username is taken, empty, or the
    // role is not one of the two above. The password is hashed before storage;
    // an empty password is refused (there is no passwordless account).
    int64_t create_user(const std::string& username, const std::string& password,
                        const std::string& display_name, const std::string& role);

    std::optional<User> find_by_username(const std::string& username);
    std::optional<User> find_by_id(int64_t id);

    // The only place a password is checked. Returns the user on success and
    // nothing on any failure — unknown user and wrong password are
    // deliberately indistinguishable to the caller.
    std::optional<User> verify_login(const std::string& username,
                                     const std::string& password);

    bool set_password(int64_t user_id, const std::string& password);

    // Also drops the user's tokens and jid mappings, so deleting an account
    // can't leave a live credential pointing at a missing row.
    bool delete_user(int64_t id);

    std::vector<User> list_users();

    // 0 means first run — main.cpp uses this to decide whether to bootstrap.
    int64_t count();

    // ── tokens (the web UI's session cookie) ──────────────────────────────────

    // Returns the opaque token to hand back as a cookie. ttl_days <= 0 stores
    // an already-expired token (used by tests).
    std::string create_token(int64_t user_id, int ttl_days = 30);

    // The authentication hot path: called on every /api/* request. Returns
    // nothing for an unknown, expired, or revoked token, or one whose user
    // has since been deleted.
    std::optional<User> resolve_token(const std::string& token);

    bool revoke_token(const std::string& token);

    // Housekeeping only — resolve_token already refuses expired tokens, so
    // this is about keeping the table small, not about correctness.
    int purge_expired_tokens();

    // ── WhatsApp identity ─────────────────────────────────────────────────────
    // The autoresponder authenticates as a service and names the sender's jid;
    // this is what turns that jid into a Funes user. An unmapped jid resolves
    // to nobody, never to a default account.

    bool map_jid(const std::string& chat_jid, int64_t user_id);
    std::optional<User> resolve_jid(const std::string& chat_jid);
    bool unmap_jid(const std::string& chat_jid);

private:
    sqlite3*   db_ = nullptr;
    std::mutex mu_;

    void migrate();
};
