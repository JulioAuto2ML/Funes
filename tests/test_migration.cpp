// =============================================================================
// tests/test_migration.cpp — a real 3.x database must survive the 4.0 upgrade
// =============================================================================
// The migration in MemoryStore::migrate() had only ever been proven by running
// it once, by hand, against a copy of a production database. That found two
// real bugs (the eager vec rebuild and INSERT OR REPLACE under a partition
// key) and both got regression tests, but the migration as a whole never did —
// so nothing in CI would notice if it started dropping rows.
//
// This builds the pre-4.0 schema from scratch rather than mutating a 4.0 one,
// because the interesting failures live in the difference between the two
// shapes: columns that do not exist yet, a UNIQUE constraint that has to be
// rebuilt rather than altered, and a vector index whose partition key is
// missing entirely.
//
// What makes this worth asserting: there is no downgrade. Funes migrates
// whatever database it opens, in place, on startup. A migration that loses a
// row loses it permanently, and the person who finds out is the one whose
// assistant has forgotten them.

#include "memory.h"
#include "sqlite3.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

// Same letter-frequency embedder the other suites use: deterministic cosine
// similarity with no model involved.
class FakeEmbedder : public EmbeddingClient {
public:
    FakeEmbedder() : EmbeddingClient("http://unused:1", "", "fake") {}
    std::vector<float> embed(const std::string& text) override {
        std::vector<float> v(26, 0.0f);
        for (char c : text) {
            if (c >= 'a' && c <= 'z') v[c - 'a'] += 1.0f;
            if (c >= 'A' && c <= 'Z') v[c - 'A'] += 1.0f;
        }
        float norm = 0;
        for (float x : v) norm += x * x;
        norm = std::sqrt(norm);
        if (norm > 0) for (float& x : v) x /= norm;
        else v[0] = 1.0f;
        return v;
    }
};

static std::string temp_db(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("funes_migrate_") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

static int exec_sql(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) { sqlite3_close(db); return 1; }
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) { std::cerr << "  sqlite: " << err << "\n"; sqlite3_free(err); }
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : 1;
}

// Reads one integer out of the database directly, for the assertions that are
// about storage rather than about the API — "is the column there at all", not
// "does list() return it".
static int64_t scalar(const std::string& path, const char* sql, int64_t fallback = -1) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) { sqlite3_close(db); return fallback; }
    sqlite3_stmt* st = nullptr;
    int64_t out = fallback;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

// ── the pre-4.0 schema, verbatim ─────────────────────────────────────────────
// Copied from what migrate() creates *before* the 4.0 block: no user_id on any
// table, UNIQUE(agent, text) on memories, single-column indexes. If this drifts
// from what 3.x actually shipped the test stops meaning anything, so it is
// spelled out here rather than derived from the current code.
static const char* const PRE4_SCHEMA = R"sql(
    CREATE TABLE meta (
        key   TEXT PRIMARY KEY,
        value TEXT NOT NULL
    );
    CREATE TABLE memories (
        id         INTEGER PRIMARY KEY,
        agent      TEXT NOT NULL,
        text       TEXT NOT NULL,
        source     TEXT NOT NULL DEFAULT 'auto',
        created_at TEXT NOT NULL DEFAULT (datetime('now')),
        recall_count     INTEGER NOT NULL DEFAULT 0,
        last_recalled_at TEXT,
        UNIQUE(agent, text)
    );
    CREATE INDEX idx_memories_agent ON memories(agent, id DESC);
    CREATE TABLE turns (
        id         INTEGER PRIMARY KEY,
        session    TEXT NOT NULL,
        agent      TEXT NOT NULL,
        role       TEXT NOT NULL,
        content    TEXT NOT NULL,
        created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
    );
    CREATE INDEX idx_turns_session ON turns(session, id);
    CREATE TABLE session_summaries (
        session    TEXT PRIMARY KEY,
        agent      TEXT NOT NULL,
        summary    TEXT NOT NULL,
        updated_at TEXT NOT NULL DEFAULT (datetime('now'))
    );
    CREATE TABLE tool_results (
        id         INTEGER PRIMARY KEY,
        session    TEXT NOT NULL,
        agent      TEXT NOT NULL,
        tool       TEXT NOT NULL,
        text       TEXT NOT NULL,
        created_at TEXT NOT NULL DEFAULT (datetime('now'))
    );
    CREATE INDEX idx_tool_results_session ON tool_results(session);
    CREATE TABLE cron_jobs (
        id          INTEGER PRIMARY KEY,
        name        TEXT NOT NULL,
        kind        TEXT NOT NULL,
        agent       TEXT NOT NULL DEFAULT '',
        task        TEXT NOT NULL DEFAULT '',
        command     TEXT NOT NULL DEFAULT '',
        schedule    TEXT NOT NULL,
        running     INTEGER NOT NULL DEFAULT 0,
        created_at  INTEGER NOT NULL,
        next_run_at INTEGER NOT NULL,
        last_run_at INTEGER NOT NULL DEFAULT 0,
        last_status TEXT NOT NULL DEFAULT '',
        last_output TEXT NOT NULL DEFAULT ''
    );
    CREATE INDEX idx_cron_jobs_due ON cron_jobs(running, next_run_at);
)sql";

// Deliberately non-contiguous ids. A migration that rebuilds a table by
// re-inserting rows is exactly the kind that renumbers them, and an id that
// changes silently breaks every URL, every DELETE /api/memories/<id>, and the
// result-store references held in existing transcripts.
static const char* const PRE4_DATA = R"sql(
    INSERT INTO memories (id, agent, text, source, created_at, recall_count) VALUES
        (3,   'funes', 'The user is a physicist',        'user', '2025-01-02 10:00:00', 7),
        (11,  'funes', 'The user prefers Rust',          'user', '2025-02-03 11:00:00', 0),
        (12,  'researcher', 'arXiv is the preprint site','auto', '2025-02-04 12:00:00', 2),
        (100, 'funes', 'The dog is called Zorbax',       'user', '2025-03-05 13:00:00', 1);

    INSERT INTO turns (id, session, agent, role, content, created_at) VALUES
        (1,  'chat-a', 'funes', 'user',      'hello there',      '2025-01-02 10:00:00.000'),
        (2,  'chat-a', 'funes', 'assistant', 'hello yourself',   '2025-01-02 10:00:01.000'),
        (9,  'chat-b', 'funes', 'user',      'what is a tensor', '2025-01-03 10:00:00.000'),
        (10, 'chat-b', 'funes', 'assistant', 'a tensor is...',   '2025-01-03 10:00:01.000');

    INSERT INTO session_summaries (session, agent, summary) VALUES
        ('chat-a', 'funes', 'They said hello.');

    INSERT INTO tool_results (id, session, agent, tool, text) VALUES
        (5, 'chat-a', 'funes', 'web_fetch', 'a large fetched page');

    INSERT INTO cron_jobs (id, name, kind, agent, task, schedule, created_at, next_run_at) VALUES
        (2, 'daily-pulse', 'agent', 'curator', 'publish the issue', '0 7 * * *', 1700000000, 1700086400);
)sql";

static int build_pre4(const std::string& path) {
    if (exec_sql(path, PRE4_SCHEMA)) return 1;
    if (exec_sql(path, PRE4_DATA)) return 1;
    return 0;
}

constexpr int64_t ADMIN = MemoryStore::ADMIN_USER_ID;
constexpr int64_t BOB   = 2;

// ── tests ────────────────────────────────────────────────────────────────────

int test_memories_survive_with_ids_intact() {
    const std::string path = temp_db("memories");
    CHECK(build_pre4(path) == 0);

    MemoryStore store(path, nullptr);

    // Nothing lost, and everything attributed to the admin — the single-user
    // install becomes account 1, so a pre-4.0 row belongs to whoever was
    // already using the box.
    CHECK(store.count(ADMIN) == 4);
    CHECK(store.count(BOB) == 0);

    auto all = store.list(ADMIN, "", 100, 0);
    CHECK(all.size() == 4);

    bool saw_3 = false, saw_100 = false;
    for (const auto& m : all) {
        CHECK(m.user_id == ADMIN);
        if (m.id == 3) {
            saw_3 = true;
            CHECK(m.text == "The user is a physicist");
            CHECK(m.agent == "funes");
            CHECK(m.source == "user");
            CHECK(m.created_at == "2025-01-02 10:00:00");
            CHECK(m.recall_count == 7);   // bookkeeping, not just the text
        }
        if (m.id == 100) {
            saw_100 = true;
            CHECK(m.text == "The dog is called Zorbax");
        }
    }
    CHECK(saw_3);      // the sparse ids specifically: a rebuild that renumbers
    CHECK(saw_100);    // rows would turn 3 and 100 into 1 and 4.

    // The per-agent scoping still works after the rebuild.
    CHECK(store.list(ADMIN, "researcher", 100, 0).size() == 1);
    return 0;
}

int test_turns_summaries_and_results_survive() {
    const std::string path = temp_db("turns");
    CHECK(build_pre4(path) == 0);

    MemoryStore store(path, nullptr);

    CHECK(store.turn_count(ADMIN, "chat-a") == 2);
    CHECK(store.turn_count(ADMIN, "chat-b") == 2);
    CHECK(store.turn_count(BOB, "chat-a") == 0);

    auto turns = store.recent_turns(ADMIN, "chat-a", 10);
    CHECK(turns.size() == 2);
    CHECK(turns[0].content == "hello there");      // oldest first
    CHECK(turns[1].content == "hello yourself");

    CHECK(store.get_summary(ADMIN, "chat-a") == "They said hello.");
    CHECK(store.get_summary(BOB, "chat-a").empty());

    // Both sessions show up in the conversation list the UI renders.
    auto sessions = store.list_sessions(ADMIN, 50);
    CHECK(sessions.size() == 2);

    CHECK(scalar(path, "SELECT user_id FROM tool_results WHERE id=5") == ADMIN);
    return 0;
}

int test_cron_jobs_survive_and_are_owned() {
    const std::string path = temp_db("cron");
    CHECK(build_pre4(path) == 0);

    MemoryStore store(path, nullptr);

    auto jobs = store.list_cron_jobs(ADMIN);
    CHECK(jobs.size() == 1);
    CHECK(jobs[0].id == 2);
    CHECK(jobs[0].name == "daily-pulse");
    CHECK(jobs[0].agent == "curator");
    CHECK(jobs[0].schedule == "0 7 * * *");
    CHECK(jobs[0].user_id == ADMIN);
    CHECK(store.list_cron_jobs(BOB).empty());
    return 0;
}

int test_unique_constraint_is_rebuilt_not_kept() {
    // The pre-4.0 constraint was UNIQUE(agent, text). Left in place it would
    // mean the second account to store a fact silently gets the first
    // account's row id back and stores nothing — a cross-user leak dressed up
    // as deduplication. SQLite cannot alter a constraint, so migrate() rebuilds
    // the table; this is the assertion that the rebuild actually happened.
    const std::string path = temp_db("unique");
    CHECK(build_pre4(path) == 0);

    MemoryStore store(path, nullptr);

    // A pre-existing text, now stored by a different account.
    int64_t bob_id = store.remember(BOB, "funes", "The user prefers Rust", "user");
    CHECK(bob_id != 0);
    CHECK(bob_id != 11);              // not the migrated admin row handed back
    CHECK(store.count(BOB) == 1);
    CHECK(store.count(ADMIN) == 4);   // and the admin's copy is untouched

    // ...while the constraint still holds within one account.
    int64_t again = store.remember(ADMIN, "funes", "The user prefers Rust", "user");
    CHECK(again == 11);
    CHECK(store.count(ADMIN) == 4);
    return 0;
}

int test_vec_index_is_partitioned_and_recall_works() {
    // A 3.x database with an embedding dimension recorded and a vec table
    // lacking the partition key. recall_semantic names v.user_id, so opening
    // this and recalling is the exact sequence that used to throw.
    const std::string path = temp_db("vec");
    CHECK(build_pre4(path) == 0);
    CHECK(exec_sql(path,
        "INSERT INTO meta(key, value) VALUES('embed_dim', '26');") == 0);

    FakeEmbedder emb;
    {
        MemoryStore store(path, &emb);
        // The texts survive the index rebuild even though the vectors do not.
        CHECK(store.count(ADMIN) == 4);

        auto hits = store.recall(ADMIN, "funes", "The dog is called Zorbax", 4);
        CHECK(!hits.empty());
        for (const auto& h : hits) CHECK(h.user_id == ADMIN);

        // A new write refills the index in the 4.0 shape.
        CHECK(store.remember(ADMIN, "funes", "The cat is called Mimo", "user") != 0);
        auto again = store.recall(ADMIN, "funes", "The cat is called Mimo", 4);
        CHECK(!again.empty());
    }

    CHECK(scalar(path, "SELECT COUNT(*) FROM meta WHERE key='vec_partitioned'") == 1);
    return 0;
}

int test_migration_is_idempotent() {
    // Restarting the server must not re-run the rebuild. If the `meta` marker
    // ever stopped guarding it, a second open would rebuild the table again —
    // harmless once, but it is also how a half-written rebuild gets retried on
    // top of itself, and the count is the cheapest way to notice.
    const std::string path = temp_db("idempotent");
    CHECK(build_pre4(path) == 0);

    for (int open = 0; open < 3; ++open) {
        MemoryStore store(path, nullptr);
        CHECK(store.count(ADMIN) == 4);
        CHECK(store.turn_count(ADMIN, "chat-a") == 2);
        CHECK(store.list_cron_jobs(ADMIN).size() == 1);
    }

    // Exactly one marker row, not one per open.
    CHECK(scalar(path, "SELECT COUNT(*) FROM meta WHERE key='schema_user_scoped'") == 1);
    CHECK(scalar(path, "SELECT COUNT(*) FROM memories") == 4);
    return 0;
}

int test_already_migrated_database_is_left_alone() {
    // The other direction: a database that is already 4.0 must not be touched
    // by the 4.0 block. Two accounts with the same text is the state the
    // pre-4.0 constraint could not represent, so if a rebuild wrongly re-ran
    // with the old constraint this is what would fail.
    const std::string path = temp_db("already");
    {
        MemoryStore store(path, nullptr);
        store.remember(ADMIN, "funes", "shared fact", "user");
        store.remember(BOB,   "funes", "shared fact", "user");
        store.append_turn(BOB, "bobs-chat", "funes", "user", "bob's private turn");
    }

    MemoryStore store(path, nullptr);
    CHECK(store.count(ADMIN) == 1);
    CHECK(store.count(BOB) == 1);
    CHECK(store.turn_count(BOB, "bobs-chat") == 1);
    CHECK(store.turn_count(ADMIN, "bobs-chat") == 0);
    return 0;
}

int test_empty_pre4_database_migrates() {
    // A 3.x install that never stored anything. The rebuild runs against zero
    // rows, which is the path where an INSERT..SELECT over an empty table and
    // a missing marker are easiest to get wrong.
    const std::string path = temp_db("empty");
    CHECK(exec_sql(path, PRE4_SCHEMA) == 0);

    MemoryStore store(path, nullptr);
    CHECK(store.count(ADMIN) == 0);
    CHECK(store.remember(ADMIN, "funes", "first fact after upgrade", "user") != 0);
    CHECK(store.count(ADMIN) == 1);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_memories_survive_with_ids_intact();
    rc |= test_turns_summaries_and_results_survive();
    rc |= test_cron_jobs_survive_and_are_owned();
    rc |= test_unique_constraint_is_rebuilt_not_kept();
    rc |= test_vec_index_is_partitioned_and_recall_works();
    rc |= test_migration_is_idempotent();
    rc |= test_already_migrated_database_is_left_alone();
    rc |= test_empty_pre4_database_migrates();
    if (rc == 0) std::cout << "test_migration: all passed\n";
    return rc;
}
