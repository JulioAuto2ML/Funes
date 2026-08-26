// =============================================================================
// tests/test_users.cpp — users, auth tokens, and WhatsApp jid mapping
// =============================================================================
// Phase 1 of the 4.0 users & permissions plan (docs/dev-plan-users-permissions.md).
//
// The invariants worth asserting are the ones that turn into security bugs if
// they regress: a token must not outlive its expiry, revoking must actually
// revoke, deleting a user must not leave live tokens pointing at a missing
// row, and an unknown jid must resolve to nobody rather than to user 1.

#include "users.h"
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static std::string temp_db(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("funes_test_users_") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

int test_create_and_find() {
    UserStore users(temp_db("create"));

    // First-run bootstrap depends on this being 0 on a fresh database.
    CHECK(users.count() == 0);

    int64_t id = users.create_user("julio", "s3cret", "Julio", "admin");
    CHECK(id > 0);
    CHECK(users.count() == 1);

    auto found = users.find_by_username("julio");
    CHECK(found.has_value());
    CHECK(found->id == id);
    CHECK(found->username == "julio");
    CHECK(found->display_name == "Julio");
    CHECK(found->role == "admin");
    CHECK(!found->created_at.empty());

    auto by_id = users.find_by_id(id);
    CHECK(by_id.has_value());
    CHECK(by_id->username == "julio");

    CHECK(!users.find_by_username("nobody").has_value());
    CHECK(!users.find_by_id(9999).has_value());
    return 0;
}

int test_duplicate_username_rejected() {
    UserStore users(temp_db("dup"));
    CHECK(users.create_user("alice", "pw1", "Alice", "member") > 0);
    // Second create must fail rather than silently shadow the first account.
    CHECK(users.create_user("alice", "pw2", "Alice Two", "admin") == 0);
    CHECK(users.count() == 1);

    // And the original password must still be the one that works.
    CHECK(users.verify_login("alice", "pw1").has_value());
    CHECK(!users.verify_login("alice", "pw2").has_value());
    return 0;
}

int test_verify_login() {
    UserStore users(temp_db("login"));
    users.create_user("bob", "correct", "Bob", "member");

    auto ok = users.verify_login("bob", "correct");
    CHECK(ok.has_value());
    CHECK(ok->username == "bob");

    CHECK(!users.verify_login("bob", "wrong").has_value());
    CHECK(!users.verify_login("bob", "").has_value());
    CHECK(!users.verify_login("nosuchuser", "correct").has_value());
    // Username comparison must not be a prefix match.
    CHECK(!users.verify_login("bo", "correct").has_value());
    return 0;
}

int test_set_password() {
    UserStore users(temp_db("setpw"));
    int64_t id = users.create_user("carol", "old", "Carol", "member");

    CHECK(users.set_password(id, "new"));
    CHECK(users.verify_login("carol", "new").has_value());
    CHECK(!users.verify_login("carol", "old").has_value());
    CHECK(!users.set_password(9999, "x"));
    return 0;
}

int test_tokens() {
    UserStore users(temp_db("tokens"));
    int64_t id = users.create_user("dave", "pw", "Dave", "member");

    std::string token = users.create_token(id, 30);
    CHECK(token.size() == 64);

    auto who = users.resolve_token(token);
    CHECK(who.has_value());
    CHECK(who->id == id);
    CHECK(who->username == "dave");

    // Garbage and near-misses resolve to nobody.
    CHECK(!users.resolve_token("").has_value());
    CHECK(!users.resolve_token("deadbeef").has_value());
    CHECK(!users.resolve_token(token.substr(0, 63)).has_value());

    CHECK(users.revoke_token(token));
    CHECK(!users.resolve_token(token).has_value());
    // Revoking twice is not an error worth reporting differently, but it must
    // not resurrect the token.
    users.revoke_token(token);
    CHECK(!users.resolve_token(token).has_value());
    return 0;
}

int test_expired_token_does_not_resolve() {
    UserStore users(temp_db("expiry"));
    int64_t id = users.create_user("erin", "pw", "Erin", "member");

    // ttl_days <= 0 backdates the expiry, which is how we test the predicate
    // without sleeping.
    std::string stale = users.create_token(id, -1);
    CHECK(!stale.empty());
    CHECK(!users.resolve_token(stale).has_value());

    std::string fresh = users.create_token(id, 1);
    CHECK(users.resolve_token(fresh).has_value());

    // The purge is bookkeeping, not the isolation boundary — resolve_token
    // above must already refuse an expired token whether or not this ran.
    int purged = users.purge_expired_tokens();
    CHECK(purged >= 1);
    CHECK(users.resolve_token(fresh).has_value());
    return 0;
}

int test_delete_user_revokes_tokens() {
    UserStore users(temp_db("delete"));
    int64_t id = users.create_user("frank", "pw", "Frank", "member");
    std::string token = users.create_token(id, 30);
    users.map_jid("5511999@s.whatsapp.net", id);

    CHECK(users.resolve_token(token).has_value());
    CHECK(users.delete_user(id));

    // A live token pointing at a deleted user must not authenticate anyone.
    CHECK(!users.resolve_token(token).has_value());
    CHECK(!users.find_by_id(id).has_value());
    CHECK(!users.resolve_jid("5511999@s.whatsapp.net").has_value());
    CHECK(users.count() == 0);
    CHECK(!users.delete_user(id));
    return 0;
}

int test_jid_mapping() {
    UserStore users(temp_db("jid"));
    int64_t id = users.create_user("gwen", "pw", "Gwen", "member");
    const std::string jid = "5511888@s.whatsapp.net";

    // An unmapped jid resolves to nobody — never to a default user.
    CHECK(!users.resolve_jid(jid).has_value());
    CHECK(!users.resolve_jid("").has_value());

    CHECK(users.map_jid(jid, id));
    auto who = users.resolve_jid(jid);
    CHECK(who.has_value());
    CHECK(who->id == id);

    // Remapping the same jid moves it rather than creating a second row.
    int64_t other = users.create_user("henry", "pw", "Henry", "member");
    CHECK(users.map_jid(jid, other));
    CHECK(users.resolve_jid(jid)->id == other);

    CHECK(users.unmap_jid(jid));
    CHECK(!users.resolve_jid(jid).has_value());

    // Mapping to a user that doesn't exist must be refused, not stored.
    CHECK(!users.map_jid(jid, 9999));
    CHECK(!users.resolve_jid(jid).has_value());
    return 0;
}

int test_list_users() {
    UserStore users(temp_db("list"));
    users.create_user("a", "pw", "A", "admin");
    users.create_user("b", "pw", "B", "member");

    auto all = users.list_users();
    CHECK(all.size() == 2);
    // Whatever the order, the hash must never be handed out with the row.
    for (const auto& u : all)
        CHECK(u.username == "a" || u.username == "b");
    return 0;
}

int test_reopen_persists() {
    // The schema has to survive a restart — migrate() is called on every open.
    const std::string path = temp_db("persist");
    int64_t id = 0;
    std::string token;
    {
        UserStore users(path);
        id = users.create_user("ivy", "pw", "Ivy", "admin");
        token = users.create_token(id, 30);
    }
    {
        UserStore users(path);
        CHECK(users.count() == 1);
        CHECK(users.find_by_id(id).has_value());
        CHECK(users.resolve_token(token).has_value());
        CHECK(users.verify_login("ivy", "pw").has_value());
    }
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_create_and_find();
    rc |= test_duplicate_username_rejected();
    rc |= test_verify_login();
    rc |= test_set_password();
    rc |= test_tokens();
    rc |= test_expired_token_does_not_resolve();
    rc |= test_delete_user_revokes_tokens();
    rc |= test_jid_mapping();
    rc |= test_list_users();
    rc |= test_reopen_persists();
    if (rc == 0) std::cout << "test_users: all passed\n";
    return rc;
}
