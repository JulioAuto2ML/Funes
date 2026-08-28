// =============================================================================
// src/core/memory.cpp — Funes memory engine (implementation)
// =============================================================================
//
// sqlite-vec notes:
//   - Statically linked; registered process-wide via sqlite3_auto_extension.
//   - vec_memories is a vec0 virtual table: KNN queries use
//       WHERE embedding MATCH ?  AND k = ?
//     with the query vector bound as a float32 blob.
//   - cosine distance is declared in the table DDL; similarity = 1 - distance.
//   - The embedding dimension is fixed per table. If the configured embedding
//     model changes dimension, the vec table is rebuilt and vectors are
//     re-created by backfill_embeddings(); memory texts are never touched.
// =============================================================================

#include "memory.h"
#include "sqlite3.h"
#include "sqlite-vec.h"
#include "text_utils.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>

// ── small sqlite helpers ──────────────────────────────────────────────────────

namespace {

void register_vec_extension() {
    static bool done = [] {
        sqlite3_auto_extension(reinterpret_cast<void (*)(void)>(sqlite3_vec_init));
        return true;
    }();
    (void)done;
}

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
    void bind_blob(int i, const void* data, size_t bytes) {
        sqlite3_bind_blob(p, i, data, static_cast<int>(bytes), SQLITE_TRANSIENT);
    }
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
    double  col_double(int i) { return sqlite3_column_double(p, i); }
    std::vector<float> col_floats(int i) {
        const void* data = sqlite3_column_blob(p, i);
        const int bytes  = sqlite3_column_bytes(p, i);
        std::vector<float> v(bytes > 0 ? bytes / sizeof(float) : 0);
        if (data && bytes > 0) std::memcpy(v.data(), data, bytes);
        return v;
    }
};

void exec_or_throw(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown sqlite error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + msg + " — SQL: " + sql);
    }
}

// ALTER TABLE ADD COLUMN is not idempotent and sqlite has no IF NOT EXISTS for
// it, so check the current shape first. Used for columns added after v1.0 to
// databases that are already in the field.
void add_column_if_missing(sqlite3* db, const char* table, const char* column,
                           const char* decl) {
    Stmt s(db, ("PRAGMA table_info(" + std::string(table) + ")").c_str());
    while (s.step())
        if (s.col_text(1) == column) return;
    exec_or_throw(db, ("ALTER TABLE " + std::string(table) + " ADD COLUMN " +
                       column + " " + decl + ";").c_str());
}

// Escape LIKE wildcards in user-supplied search text.
std::string like_pattern(const std::string& query) {
    std::string out = "%";
    for (char c : query) {
        if (c == '%' || c == '_' || c == '\\') out += '\\';
        out += c;
    }
    out += '%';
    return out;
}

} // namespace

// ── construction / schema ─────────────────────────────────────────────────────

MemoryStore::MemoryStore(const std::string& db_path, EmbeddingClient* embedder)
    : embedder_(embedder)
{
    register_vec_extension();

    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw std::runtime_error("Cannot open memory database '" + db_path + "': " + msg);
    }

    exec_or_throw(db_, "PRAGMA journal_mode=WAL;");
    exec_or_throw(db_, "PRAGMA busy_timeout=5000;");
    exec_or_throw(db_, "PRAGMA foreign_keys=ON;");
    migrate();
}

MemoryStore::~MemoryStore() {
    if (db_) sqlite3_close(db_);
}

void MemoryStore::migrate() {
    exec_or_throw(db_, R"sql(
        CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS memories (
            id         INTEGER PRIMARY KEY,
            agent      TEXT NOT NULL,
            text       TEXT NOT NULL,
            source     TEXT NOT NULL DEFAULT 'auto',
            created_at TEXT NOT NULL DEFAULT (datetime('now')),
            UNIQUE(agent, text)
        );
        CREATE INDEX IF NOT EXISTS idx_memories_agent ON memories(agent, id DESC);
        CREATE TABLE IF NOT EXISTS turns (
            id         INTEGER PRIMARY KEY,
            session    TEXT NOT NULL,
            agent      TEXT NOT NULL,
            role       TEXT NOT NULL,
            content    TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now'))
        );
        CREATE INDEX IF NOT EXISTS idx_turns_session ON turns(session, id);
        CREATE TABLE IF NOT EXISTS session_summaries (
            session    TEXT PRIMARY KEY,
            agent      TEXT NOT NULL,
            summary    TEXT NOT NULL,
            updated_at TEXT NOT NULL DEFAULT (datetime('now'))
        );
        CREATE TABLE IF NOT EXISTS tool_results (
            id         INTEGER PRIMARY KEY,
            session    TEXT NOT NULL,
            agent      TEXT NOT NULL,
            tool       TEXT NOT NULL,
            text       TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_tool_results_session ON tool_results(session);
        CREATE TABLE IF NOT EXISTS cron_jobs (
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
        CREATE INDEX IF NOT EXISTS idx_cron_jobs_due ON cron_jobs(running, next_run_at);
    )sql");

    // Recall bookkeeping, added in 2.0 — consolidate() prunes on it, so it has
    // to exist on databases created before it did.
    add_column_if_missing(db_, "memories", "recall_count", "INTEGER NOT NULL DEFAULT 0");
    add_column_if_missing(db_, "memories", "last_recalled_at", "TEXT");

    // ── 4.0: multi-user ───────────────────────────────────────────────────────
    // DEFAULT 1 attributes every pre-4.0 row to the admin account, which is
    // what the single-user install becomes. See ADMIN_USER_ID in memory.h.
    const char* const USER_ID_DECL = "INTEGER NOT NULL DEFAULT 1";
    add_column_if_missing(db_, "memories",          "user_id", USER_ID_DECL);
    add_column_if_missing(db_, "turns",             "user_id", USER_ID_DECL);
    add_column_if_missing(db_, "session_summaries", "user_id", USER_ID_DECL);
    add_column_if_missing(db_, "tool_results",      "user_id", USER_ID_DECL);
    add_column_if_missing(db_, "cron_jobs",         "user_id", USER_ID_DECL);

    // The pre-4.0 unique constraint was UNIQUE(agent, text), which would make
    // one user storing a fact block every other user from storing it — and
    // silently hand back the first user's row id. SQLite cannot alter a
    // constraint in place, so the table is rebuilt. Guarded by a marker in
    // `meta` rather than by inspecting the constraint, and wrapped in one
    // transaction: a crash mid-rebuild must not leave the memories behind.
    {
        Stmt check(db_, "SELECT value FROM meta WHERE key='schema_user_scoped'");
        const bool done = check.step();
        if (!done) {
            exec_or_throw(db_, R"sql(
                BEGIN IMMEDIATE;
                CREATE TABLE memories_new (
                    id         INTEGER PRIMARY KEY,
                    user_id    INTEGER NOT NULL DEFAULT 1,
                    agent      TEXT NOT NULL,
                    text       TEXT NOT NULL,
                    source     TEXT NOT NULL DEFAULT 'auto',
                    created_at TEXT NOT NULL DEFAULT (datetime('now')),
                    recall_count     INTEGER NOT NULL DEFAULT 0,
                    last_recalled_at TEXT,
                    UNIQUE(user_id, agent, text)
                );
                INSERT INTO memories_new
                    (id, user_id, agent, text, source, created_at,
                     recall_count, last_recalled_at)
                SELECT id, user_id, agent, text, source, created_at,
                       recall_count, last_recalled_at
                FROM memories;
                DROP TABLE memories;
                ALTER TABLE memories_new RENAME TO memories;
                CREATE INDEX IF NOT EXISTS idx_memories_user_agent
                    ON memories(user_id, agent, id DESC);
                INSERT INTO meta(key, value) VALUES('schema_user_scoped', '1');
                COMMIT;
            )sql");
            std::cerr << "[memory] migrated to per-user memory scoping\n";
        }
    }

    // session_summaries had the same shape of bug the memories rebuild above
    // fixes, and was missed: `session` alone is the primary key, so there is
    // one summary row per session name across every account. The insert
    // guarded its UPDATE arm on user_id, which stopped one account overwriting
    // another's — but the cost was that the second account silently got no
    // summary at all. Its conflicting INSERT matched a row it did not own, the
    // UPDATE arm was skipped, and nothing was stored. Not a leak; a quiet loss
    // of context compression for whoever picked the session name second.
    {
        Stmt check(db_, "SELECT value FROM meta WHERE key='schema_summaries_user_scoped'");
        if (!check.step()) {
            exec_or_throw(db_, R"sql(
                BEGIN IMMEDIATE;
                CREATE TABLE session_summaries_new (
                    session    TEXT NOT NULL,
                    user_id    INTEGER NOT NULL DEFAULT 1,
                    agent      TEXT NOT NULL,
                    summary    TEXT NOT NULL,
                    updated_at TEXT NOT NULL DEFAULT (datetime('now')),
                    PRIMARY KEY (user_id, session)
                );
                INSERT INTO session_summaries_new (session, user_id, agent, summary, updated_at)
                SELECT session, user_id, agent, summary, updated_at FROM session_summaries;
                DROP TABLE session_summaries;
                ALTER TABLE session_summaries_new RENAME TO session_summaries;
                INSERT INTO meta(key, value) VALUES('schema_summaries_user_scoped', '1');
                COMMIT;
            )sql");
            std::cerr << "[memory] migrated session summaries to per-user scoping\n";
        }
    }

    // Indexes for the new predicates. The old single-column ones are left in
    // place; SQLite picks whichever is cheaper and dropping them buys nothing.
    exec_or_throw(db_, R"sql(
        CREATE INDEX IF NOT EXISTS idx_turns_user_session
            ON turns(user_id, session, id);
        CREATE INDEX IF NOT EXISTS idx_tool_results_user_session
            ON tool_results(user_id, session);
    )sql");

    // Restore the vec table dimension recorded by a previous run.
    {
        Stmt s(db_, "SELECT value FROM meta WHERE key='embed_dim'");
        if (s.step())
            dim_ = std::stoi(s.col_text(0));
    }

    // Rebuild the vector index here rather than waiting for the next write.
    // ensure_vec_table is otherwise only reached through insert_vector, but a
    // recall can come first — and recall_semantic's query names v.user_id,
    // which a pre-4.0 vec table does not have. That is a failed prepare, not a
    // graceful degradation: the statement throws and takes the agent run with
    // it. Reproduced against a copy of a real 3.x database before shipping.
    if (dim_ != 0) ensure_vec_table(dim_);
}

void MemoryStore::ensure_vec_table(int dim) {
    // The 4.0 vec table adds a user_id partition key. A pre-4.0 table has the
    // right dimension but the wrong shape, so the dimension check alone would
    // keep it forever — hence the separate marker.
    Stmt marker(db_, "SELECT value FROM meta WHERE key='vec_partitioned'");
    const bool partitioned = marker.step();

    if (dim_ == dim && partitioned) return;

    if (dim_ != 0 && dim_ != dim) {
        std::cerr << "[memory] embedding dimension changed " << dim_ << " → " << dim
                  << "; rebuilding vector index (memory texts are preserved)\n";
        exec_or_throw(db_, "DROP TABLE IF EXISTS vec_memories;");
    } else if (!partitioned) {
        std::cerr << "[memory] rebuilding vector index with per-user partitioning "
                     "(memory texts are preserved; vectors refill in the background)\n";
        exec_or_throw(db_, "DROP TABLE IF EXISTS vec_memories;");
    }

    // user_id is a PARTITION KEY, not an ordinary column: vec0 refuses to
    // combine MATCH with an arbitrary WHERE clause, but a partition key it
    // will — and it prunes whole partitions rather than filtering after the
    // fact. That distinction is the whole point here. Post-filtering a shared
    // KNN result, which is what recall_semantic did when there was only one
    // user, silently degrades as users are added: the k*8 candidate pool is
    // drawn from everybody, so a busy account can crowd a quiet one out of
    // its own top-k and recall just quietly gets worse.
    const std::string ddl =
        "CREATE VIRTUAL TABLE IF NOT EXISTS vec_memories USING vec0("
        "  memory_id INTEGER PRIMARY KEY,"
        "  user_id INTEGER PARTITION KEY,"
        "  embedding float[" + std::to_string(dim) + "] distance_metric=cosine"
        ");";
    exec_or_throw(db_, ddl.c_str());
    exec_or_throw(db_,
        "INSERT OR REPLACE INTO meta(key, value) VALUES('vec_partitioned', '1');");

    Stmt s(db_, "INSERT OR REPLACE INTO meta(key, value) VALUES('embed_dim', ?)");
    s.bind_text(1, std::to_string(dim));
    s.step();
    dim_ = dim;
}

// ── embedding helpers ─────────────────────────────────────────────────────────

bool MemoryStore::try_embed(const std::string& text, std::vector<float>& out) {
    if (!embedder_) return false;
    try {
        out = embedder_->embed(text);
        embedder_ok_ = true;
        return !out.empty();
    } catch (const std::exception& e) {
        if (embedder_ok_)
            std::cerr << "[memory] embedding unavailable, falling back to keyword "
                         "search: " << e.what() << "\n";
        embedder_ok_ = false;
        return false;
    }
}

void MemoryStore::insert_vector(int64_t user_id, int64_t memory_id,
                                const std::vector<float>& vec) {
    ensure_vec_table(static_cast<int>(vec.size()));
    // DELETE then INSERT, not INSERT OR REPLACE: a vec0 table with a PARTITION
    // KEY rejects the replace with "UNIQUE constraint failed on v primary key"
    // when the row already exists. The only caller that hits an existing row
    // is consolidation re-vectorising a merged memory, so the symptom was a
    // merged memory silently losing its embedding — recall quietly dropping
    // to keyword for exactly the rows consolidation had just decided were the
    // important ones. Verified against sqlite-vec directly.
    {
        Stmt del(db_, "DELETE FROM vec_memories WHERE memory_id = ?");
        del.bind_int64(1, memory_id);
        del.step();
    }
    Stmt s(db_, "INSERT INTO vec_memories(memory_id, user_id, embedding) "
                "VALUES(?, ?, ?)");
    s.bind_int64(1, memory_id);
    s.bind_int64(2, user_id);
    s.bind_blob(3, vec.data(), vec.size() * sizeof(float));
    s.step();
}

// ── long-term memory ──────────────────────────────────────────────────────────

int64_t MemoryStore::remember(int64_t user_id, const std::string& agent,
                              const std::string& text, const std::string& source) {
    if (agent.empty() || text.empty())
        throw std::runtime_error("remember: agent and text must be non-empty");
    if (user_id <= 0)
        throw std::runtime_error("remember: a valid user_id is required");

    std::vector<float> vec;
    const bool have_vec = try_embed(text, vec);  // network call outside the lock

    std::lock_guard<std::mutex> lock(mu_);

    {
        Stmt s(db_, "INSERT OR IGNORE INTO memories(user_id, agent, text, source) "
                    "VALUES(?,?,?,?)");
        s.bind_int64(1, user_id);
        s.bind_text(2, agent);
        s.bind_text(3, text);
        s.bind_text(4, source);
        s.step();
    }

    int64_t id = sqlite3_last_insert_rowid(db_);
    if (sqlite3_changes(db_) == 0) {
        // Duplicate (user_id, agent, text): fetch this user's existing row.
        Stmt s(db_, "SELECT id FROM memories WHERE user_id=? AND agent=? AND text=?");
        s.bind_int64(1, user_id);
        s.bind_text(2, agent);
        s.bind_text(3, text);
        if (s.step()) id = s.col_int64(0);
        return id;  // vector already present (or backfill will handle it)
    }

    if (have_vec) {
        try { insert_vector(user_id, id, vec); }
        catch (const std::exception& e) {
            std::cerr << "[memory] vector insert failed for memory " << id
                      << ": " << e.what() << "\n";
        }
    }
    return id;
}

std::vector<MemoryStore::Memory> MemoryStore::recall(int64_t user_id,
                                                     const std::string& agent,
                                                     const std::string& query, int k,
                                                     bool touch) {
    if (query.empty() || k <= 0 || user_id <= 0) return {};

    std::vector<float> qvec;
    if (try_embed(query, qvec)) {
        std::lock_guard<std::mutex> lock(mu_);
        if (dim_ == static_cast<int>(qvec.size())) {
            auto results = recall_semantic(user_id, agent, qvec, k);
            if (!results.empty()) {
                if (touch) touch_recalled(results);
                return results;
            }
        }
        // No vec table yet, dimension mismatch, or empty index → keyword.
        auto results = recall_keyword(user_id, agent, query, k);
        if (touch) touch_recalled(results);
        return results;
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto results = recall_keyword(user_id, agent, query, k);
    if (touch) touch_recalled(results);
    return results;
}

// Called with mu_ held, from every path that hands memories back to an agent.
// This is what makes "never recalled" mean something to consolidate()'s prune
// step — an auto memory that has been useful at least once is kept.
void MemoryStore::touch_recalled(const std::vector<Memory>& hits) {
    if (hits.empty()) return;
    Stmt s(db_, "UPDATE memories SET recall_count = recall_count + 1, "
                "last_recalled_at = datetime('now') WHERE id = ?");
    for (const auto& m : hits) {
        sqlite3_reset(s.p);
        s.bind_int64(1, m.id);
        s.step();
    }
}

// A deliberate fact outranks a passive conversation log at the same raw
// similarity — see the Memory::score doc comment. Multiplicative rather than
// additive so it scales with how confident the raw similarity already is,
// instead of being able to drag an unrelated memory into relevance on its own.
double MemoryStore::source_weight(const std::string& source) {
    // 1.15 measured too weak live: a genuinely on-topic "auto" log (the
    // query itself *is* about names, so a past exchange about names scores
    // high on raw similarity, not just from clutter) still buried "El
    // usuario se llama Julio." outside a top-4 window. 1.3 was the smallest
    // bump that cleared the observed gap, checked against real scores.
    if (source == "tool" || source == "user") return 1.3;   // deliberately taught
    if (source == "consolidated")              return 1.15;  // survived a merge review
    return 1.0;                                              // "auto" and anything else
}

std::vector<MemoryStore::Memory> MemoryStore::recall_semantic(
    int64_t user_id, const std::string& agent, const std::vector<float>& qvec, int k) {
    // The user_id predicate is a vec0 PARTITION KEY (see ensure_vec_table), so
    // unlike an ordinary column it *can* sit alongside MATCH — the KNN runs
    // inside this user's partition instead of across everybody's and being
    // filtered afterwards. That keeps the candidate pool below a per-user
    // budget rather than a shared one, which is what stops recall quality
    // decaying as accounts are added.
    //
    // The agent filter is still applied after the fact (agent is not a
    // partition key), so the over-fetch stays: it has to survive that filter
    // *and* give source-weighted re-ranking a real pool to work with —
    // stopping at exactly k in raw-similarity order would fetch the pool but
    // never let a lower-similarity/higher-weight memory displace anything.
    const int fetch_k = k * 8;

    Stmt s(db_, R"sql(
        SELECT m.id, m.agent, m.text, m.source, m.created_at, v.distance,
               m.recall_count, m.user_id
        FROM vec_memories v
        JOIN memories m ON m.id = v.memory_id
        WHERE v.user_id = ? AND v.embedding MATCH ? AND v.k = ?
        ORDER BY v.distance
    )sql");
    s.bind_int64(1, user_id);
    s.bind_blob(2, qvec.data(), qvec.size() * sizeof(float));
    s.bind_int64(3, fetch_k);

    std::vector<Memory> out;
    while (s.step()) {
        if (!agent.empty() && s.col_text(1) != agent) continue;
        // Belt and braces: the partition key already scoped the KNN, but the
        // join could still surface a row whose vector was written under the
        // wrong partition by an older build. Cross-user leakage is the one
        // failure here that must not be possible.
        if (s.col_int64(7) != user_id) continue;
        Memory m;
        m.id         = s.col_int64(0);
        m.agent      = s.col_text(1);
        m.text       = s.col_text(2);
        m.source     = s.col_text(3);
        m.created_at   = s.col_text(4);
        double similarity = 1.0 - s.col_double(5);  // cosine distance → similarity
        m.score         = similarity * source_weight(m.source);
        m.recall_count  = s.col_int64(6);
        m.user_id       = s.col_int64(7);
        out.push_back(std::move(m));
    }

    std::stable_sort(out.begin(), out.end(),
                      [](const Memory& a, const Memory& b) { return a.score > b.score; });
    if (static_cast<int>(out.size()) > k) out.resize(k);
    return out;
}

std::vector<MemoryStore::Memory> MemoryStore::recall_keyword(
    int64_t user_id, const std::string& agent, const std::string& query, int k) {
    std::string sql =
        "SELECT id, agent, text, source, created_at, recall_count, user_id FROM memories "
        "WHERE user_id = ? AND text LIKE ? ESCAPE '\\'";
    if (!agent.empty()) sql += " AND agent = ?";
    sql += " ORDER BY id DESC LIMIT ?";

    Stmt s(db_, sql.c_str());
    int i = 1;
    s.bind_int64(i++, user_id);
    s.bind_text(i++, like_pattern(query));
    if (!agent.empty()) s.bind_text(i++, agent);
    s.bind_int64(i, k);

    std::vector<Memory> out;
    while (s.step()) {
        Memory m;
        m.id           = s.col_int64(0);
        m.agent        = s.col_text(1);
        m.text         = s.col_text(2);
        m.source       = s.col_text(3);
        m.created_at   = s.col_text(4);
        m.recall_count = s.col_int64(5);
        m.user_id      = s.col_int64(6);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<MemoryStore::Memory> MemoryStore::list(int64_t user_id,
                                                   const std::string& agent,
                                                   int limit, int offset) {
    std::lock_guard<std::mutex> lock(mu_);

    std::string sql = "SELECT id, agent, text, source, created_at, recall_count, user_id "
                      "FROM memories WHERE user_id = ?";
    if (!agent.empty()) sql += " AND agent = ?";
    sql += " ORDER BY id DESC LIMIT ? OFFSET ?";

    Stmt s(db_, sql.c_str());
    int i = 1;
    s.bind_int64(i++, user_id);
    if (!agent.empty()) s.bind_text(i++, agent);
    s.bind_int64(i++, limit);
    s.bind_int64(i, offset);

    std::vector<Memory> out;
    while (s.step()) {
        Memory m;
        m.id           = s.col_int64(0);
        m.agent        = s.col_text(1);
        m.text         = s.col_text(2);
        m.source       = s.col_text(3);
        m.created_at   = s.col_text(4);
        m.recall_count = s.col_int64(5);
        m.user_id      = s.col_int64(6);
        out.push_back(std::move(m));
    }
    return out;
}

bool MemoryStore::forget(int64_t user_id, int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    // The ownership predicate is on the DELETE itself rather than a SELECT
    // first: a check-then-act would be a race, and this way "not yours" and
    // "not there" are the same zero-rows-changed result the caller reports as
    // 404 — so the endpoint can't be used to probe which ids exist.
    Stmt s(db_, "DELETE FROM memories WHERE id = ? AND user_id = ?");
    s.bind_int64(1, id);
    s.bind_int64(2, user_id);
    s.step();
    const bool deleted = sqlite3_changes(db_) > 0;

    // Only drop the vector once the row it belongs to is confirmed gone —
    // otherwise a delete aimed at someone else's memory would still strip
    // their embedding and silently downgrade them to keyword recall.
    if (deleted && dim_ != 0) {
        Stmt v(db_, "DELETE FROM vec_memories WHERE memory_id = ?");
        v.bind_int64(1, id);
        v.step();
    }
    return deleted;
}

int64_t MemoryStore::count(int64_t user_id, const std::string& agent) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string sql = "SELECT COUNT(*) FROM memories WHERE 1=1";
    if (user_id >= 0)       sql += " AND user_id = ?";
    if (!agent.empty())     sql += " AND agent = ?";
    Stmt s(db_, sql.c_str());
    int i = 1;
    if (user_id >= 0)   s.bind_int64(i++, user_id);
    if (!agent.empty()) s.bind_text(i, agent);
    s.step();
    return s.col_int64(0);
}

std::vector<int64_t> MemoryStore::user_ids_with_memories() {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT DISTINCT user_id FROM memories ORDER BY user_id");
    std::vector<int64_t> out;
    while (s.step()) out.push_back(s.col_int64(0));
    return out;
}

int64_t MemoryStore::count_missing_vectors() {
    if (!embedder_) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    // dim_ == 0 means the vec table has not been sized yet (no vector has ever
    // been written), so every memory is missing one.
    Stmt s(db_, dim_ == 0
        ? "SELECT COUNT(*) FROM memories"
        : "SELECT COUNT(*) FROM memories m "
          "LEFT JOIN vec_memories v ON v.memory_id = m.id "
          "WHERE v.memory_id IS NULL");
    return s.step() ? s.col_int64(0) : 0;
}

size_t MemoryStore::backfill_embeddings(size_t max_items) {
    if (!embedder_) return 0;

    // Collect candidates under the lock, embed outside it. user_id travels
    // with each row so the vector lands in its owner's partition — a backfill
    // that guessed would quietly file everyone's vectors under one user and
    // make recall miss for the rest.
    struct Pending { int64_t id; int64_t user_id; std::string text; };
    std::vector<Pending> missing;
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::string sql = (dim_ == 0)
            ? "SELECT id, user_id, text FROM memories ORDER BY id LIMIT ?"
            : "SELECT m.id, m.user_id, m.text FROM memories m "
              "LEFT JOIN vec_memories v ON v.memory_id = m.id "
              "WHERE v.memory_id IS NULL ORDER BY m.id LIMIT ?";
        Stmt s(db_, sql.c_str());
        s.bind_int64(1, static_cast<int64_t>(max_items));
        while (s.step())
            missing.push_back({s.col_int64(0), s.col_int64(1), s.col_text(2)});
    }

    size_t done = 0;
    for (const auto& p : missing) {
        std::vector<float> vec;
        if (!try_embed(p.text, vec)) break;  // endpoint down — stop, retry later
        std::lock_guard<std::mutex> lock(mu_);
        try {
            insert_vector(p.user_id, p.id, vec);
            ++done;
        } catch (const std::exception& e) {
            std::cerr << "[memory] backfill failed for memory " << p.id
                      << ": " << e.what() << "\n";
            break;
        }
    }
    return done;
}

// ── consolidation ─────────────────────────────────────────────────────────────

int MemoryStore::prune_stale(const ConsolidationOptions& opt) {
    if (opt.prune_after_days < 0) return 0;   // pruning disabled

    std::lock_guard<std::mutex> lock(mu_);

    // source='user' is protected: an explicit `remember` call is a decision,
    // not a byproduct, and age says nothing about whether it still matters.
    // Consolidated rows are protected for the same reason — they were merged
    // deliberately, and re-merging then deleting would lose the fact entirely.
    std::string sql =
        "SELECT id FROM memories WHERE source = 'auto' AND recall_count = 0 "
        "AND created_at <= datetime('now', ?)";   // <= so days=0 means "any age"
    if (opt.user_id >= 0)   sql += " AND user_id = ?";
    if (!opt.agent.empty()) sql += " AND agent = ?";

    std::vector<int64_t> ids;
    {
        Stmt s(db_, sql.c_str());
        int i = 1;
        s.bind_text(i++, "-" + std::to_string(opt.prune_after_days) + " days");
        if (opt.user_id >= 0)   s.bind_int64(i++, opt.user_id);
        if (!opt.agent.empty()) s.bind_text(i, opt.agent);
        while (s.step()) ids.push_back(s.col_int64(0));
    }
    if (ids.empty()) return 0;

    Stmt del_mem(db_, "DELETE FROM memories WHERE id = ?");
    for (int64_t id : ids) {
        if (dim_ != 0) {
            Stmt v(db_, "DELETE FROM vec_memories WHERE memory_id = ?");
            v.bind_int64(1, id);
            v.step();
        }
        sqlite3_reset(del_mem.p);
        del_mem.bind_int64(1, id);
        del_mem.step();
    }
    return static_cast<int>(ids.size());
}

MemoryStore::ConsolidationReport MemoryStore::consolidate(
    const MergeFn& merge, const ConsolidationOptions& opt) {
    ConsolidationReport report;

    // Consolidating "every user" means consolidating each user's pool in turn,
    // never one pool spanning accounts: two people stating the same fact are
    // two facts, and handing both to the model would write one person's
    // wording into the other's memory.
    if (opt.user_id < 0) {
        for (int64_t uid : user_ids_with_memories()) {
            ConsolidationOptions per_user = opt;
            per_user.user_id = uid;
            ConsolidationReport r = consolidate(merge, per_user);
            report.clusters_seen += r.clusters_seen;
            report.merged       += r.merged;
            report.pruned       += r.pruned;
            report.kept         += r.kept;
        }
        return report;
    }

    // Step 1 — near-duplicate merge. Needs vectors: in keyword-only mode there
    // is nothing to cluster on, so this is skipped and only the prune runs.
    if (merge && semantic_available() && dim_ != 0) {
        std::vector<int64_t> candidates;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::string sql = "SELECT m.id FROM memories m "
                              "JOIN vec_memories v ON v.memory_id = m.id "
                              "WHERE m.user_id = ?";
            if (!opt.agent.empty()) sql += " AND m.agent = ?";
            sql += " ORDER BY m.id";
            Stmt s(db_, sql.c_str());
            s.bind_int64(1, opt.user_id);
            if (!opt.agent.empty()) s.bind_text(2, opt.agent);
            while (s.step()) candidates.push_back(s.col_int64(0));
        }

        std::set<int64_t> claimed;
        for (int64_t seed : candidates) {
            if (report.clusters_seen >= static_cast<int>(opt.max_clusters)) break;
            if (claimed.count(seed)) continue;

            // Neighbours of `seed` above the similarity floor, same agent.
            std::vector<Memory> cluster;
            {
                std::lock_guard<std::mutex> lock(mu_);
                std::vector<float> vec;
                {
                    Stmt s(db_, "SELECT embedding FROM vec_memories WHERE memory_id = ?");
                    s.bind_int64(1, seed);
                    if (!s.step()) continue;
                    vec = s.col_floats(0);
                }
                if (vec.empty() || static_cast<int>(vec.size()) != dim_) continue;

                Stmt s(db_, R"sql(
                    SELECT m.id, m.agent, m.text, m.source, m.created_at, v.distance
                    FROM vec_memories v
                    JOIN memories m ON m.id = v.memory_id
                    WHERE v.user_id = ? AND v.embedding MATCH ? AND v.k = 8
                    ORDER BY v.distance
                )sql");
                s.bind_int64(1, opt.user_id);
                s.bind_blob(2, vec.data(), vec.size() * sizeof(float));

                std::string seed_agent;
                while (s.step()) {
                    Memory m;
                    m.id         = s.col_int64(0);
                    m.agent      = s.col_text(1);
                    m.text       = s.col_text(2);
                    m.source     = s.col_text(3);
                    m.created_at = s.col_text(4);
                    m.score      = 1.0 - s.col_double(5);
                    if (m.id == seed) seed_agent = m.agent;
                    if (claimed.count(m.id)) continue;
                    if (m.score < opt.min_similarity) continue;
                    cluster.push_back(std::move(m));
                }
                // Cross-agent near-duplicates are not duplicates: the same
                // sentence means different things in two agents' histories.
                cluster.erase(std::remove_if(cluster.begin(), cluster.end(),
                                             [&](const Memory& m) { return m.agent != seed_agent; }),
                              cluster.end());
            }

            if (cluster.size() < 2) continue;
            ++report.clusters_seen;
            for (const auto& m : cluster) claimed.insert(m.id);

            std::vector<std::string> texts;
            for (const auto& m : cluster) texts.push_back(m.text);

            std::string merged;
            try {
                merged = merge(texts);
            } catch (const std::exception& e) {
                // A failed merge call must not abort the run: the cluster is
                // untouched and the next run will see it again.
                std::cerr << "[memory] consolidation merge failed: " << e.what() << "\n";
                continue;
            }
            if (merged.empty() || merged.rfind("KEEP ALL", 0) == 0) {
                ++report.kept;
                continue;
            }

            std::vector<float> merged_vec;
            const bool have_vec = try_embed(merged, merged_vec);  // outside the lock

            std::lock_guard<std::mutex> lock(mu_);
            try {
                exec_or_throw(db_, "BEGIN IMMEDIATE");
                int64_t new_id = 0;
                {
                    Stmt s(db_, "INSERT OR IGNORE INTO memories(user_id, agent, text, source) "
                                "VALUES(?,?,?,'consolidated')");
                    s.bind_int64(1, opt.user_id);
                    s.bind_text(2, cluster.front().agent);
                    s.bind_text(3, merged);
                    s.step();
                    new_id = sqlite3_last_insert_rowid(db_);
                    if (sqlite3_changes(db_) == 0) {
                        // The merged sentence already exists verbatim: keep the
                        // existing row and still drop the originals.
                        Stmt e(db_, "SELECT id FROM memories WHERE user_id=? AND agent=? "
                                    "AND text=?");
                        e.bind_int64(1, opt.user_id);
                        e.bind_text(2, cluster.front().agent);
                        e.bind_text(3, merged);
                        if (e.step()) new_id = e.col_int64(0);
                    }
                }
                for (const auto& m : cluster) {
                    if (m.id == new_id) continue;
                    Stmt v(db_, "DELETE FROM vec_memories WHERE memory_id = ?");
                    v.bind_int64(1, m.id);
                    v.step();
                    Stmt d(db_, "DELETE FROM memories WHERE id = ?");
                    d.bind_int64(1, m.id);
                    d.step();
                }
                exec_or_throw(db_, "COMMIT");
                report.merged += static_cast<int>(cluster.size());

                // Outside the transaction on purpose: a missing vector is
                // recoverable (backfill_embeddings picks it up), a half-applied
                // merge is not.
                if (have_vec) {
                    try { insert_vector(opt.user_id, new_id, merged_vec); }
                    catch (const std::exception& e) {
                        std::cerr << "[memory] merged-vector insert failed for " << new_id
                                  << ": " << e.what() << "\n";
                    }
                }
            } catch (const std::exception& e) {
                sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
                std::cerr << "[memory] consolidation cluster rolled back: " << e.what() << "\n";
            }
        }
    }

    // Step 2 — prune. Runs with or without an embedder.
    report.pruned = prune_stale(opt);
    return report;
}

// ── tool results ──────────────────────────────────────────────────────────────

int64_t MemoryStore::store_result(int64_t user_id, const std::string& session,
                                  const std::string& agent, const std::string& tool,
                                  const std::string& text) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "INSERT INTO tool_results(user_id, session, agent, tool, text) "
                "VALUES(?,?,?,?,?)");
    s.bind_int64(1, user_id);
    s.bind_text(2, session);
    s.bind_text(3, agent);
    s.bind_text(4, tool);
    s.bind_text(5, text);
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

std::optional<std::string> MemoryStore::get_result(int64_t user_id,
                                                   const std::string& session,
                                                   int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    // Both predicates are isolation boundaries, not optimisations: a result id
    // from another conversation must read as "not found", and so must one from
    // another account. Session alone was enough while there was one user;
    // session ids are client-supplied strings, so two accounts can hold the
    // same one.
    Stmt s(db_, "SELECT text FROM tool_results WHERE id = ? AND session = ? "
                "AND user_id = ?");
    s.bind_int64(1, id);
    s.bind_text(2, session);
    s.bind_int64(3, user_id);
    if (!s.step()) return std::nullopt;
    return s.col_text(0);
}

int MemoryStore::prune_results(int64_t user_id, const std::string& session) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "DELETE FROM tool_results WHERE session = ? AND user_id = ?");
    s.bind_text(1, session);
    s.bind_int64(2, user_id);
    s.step();
    return sqlite3_changes(db_);
}

int MemoryStore::prune_results_older_than(int days) {
    if (days < 0) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    // <= rather than <, so days=0 means "drop them all" — a usable switch, and
    // the second of slack it adds at any other value is meaningless here.
    Stmt s(db_, "DELETE FROM tool_results WHERE created_at <= datetime('now', ?)");
    s.bind_text(1, "-" + std::to_string(days) + " days");
    s.step();
    return sqlite3_changes(db_);
}

// ── conversation history ──────────────────────────────────────────────────────

void MemoryStore::append_turn(int64_t user_id, const std::string& session,
                              const std::string& agent, const std::string& role,
                              const std::string& content) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "INSERT INTO turns(user_id, session, agent, role, content) "
                "VALUES(?,?,?,?,?)");
    s.bind_int64(1, user_id);
    s.bind_text(2, session);
    s.bind_text(3, agent);
    s.bind_text(4, role);
    s.bind_text(5, content);
    s.step();
}

std::vector<ChatMessage> MemoryStore::recent_turns(int64_t user_id,
                                                   const std::string& session, int n) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, R"sql(
        SELECT role, content FROM (
            SELECT id, role, content FROM turns
            WHERE user_id = ? AND session = ? AND role IN ('user','assistant')
            ORDER BY id DESC LIMIT ?
        ) ORDER BY id ASC
    )sql");
    s.bind_int64(1, user_id);
    s.bind_text(2, session);
    s.bind_int64(3, n);

    std::vector<ChatMessage> out;
    while (s.step()) {
        ChatMessage m;
        m.role    = s.col_text(0);
        m.content = s.col_text(1);
        out.push_back(std::move(m));
    }
    return out;
}

int64_t MemoryStore::turn_count(int64_t user_id, const std::string& session) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT COUNT(*) FROM turns WHERE user_id = ? AND session = ? "
                "AND role IN ('user','assistant')");
    s.bind_int64(1, user_id);
    s.bind_text(2, session);
    s.step();
    return s.col_int64(0);
}

void MemoryStore::prune_turns(int64_t user_id, const std::string& session, int keep) {
    if (keep < 0) keep = 0;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, R"sql(
        DELETE FROM turns WHERE user_id = ? AND session = ? AND id NOT IN (
            SELECT id FROM turns WHERE user_id = ? AND session = ?
            ORDER BY id DESC LIMIT ?
        )
    )sql");
    s.bind_int64(1, user_id);
    s.bind_text(2, session);
    s.bind_int64(3, user_id);
    s.bind_text(4, session);
    s.bind_int64(5, keep);
    s.step();
}

MemoryStore::CronCleanup MemoryStore::cleanup_cron_history(const CronCleanupOptions& opt) {
    CronCleanup report;
    const int64_t user_id = opt.user_id;

    // Collected first, deleted after — delete_session() and forget() each take
    // the mutex themselves, so iterating a live statement while calling them
    // would deadlock. The set is small (one row per firing) so holding it in
    // memory costs nothing.
    struct Target { int64_t user_id; std::string session; int64_t turns; };
    std::vector<Target> sessions;
    std::vector<std::pair<int64_t, int64_t>> memories;   // (user_id, memory id)
    {
        std::lock_guard<std::mutex> lock(mu_);
        {
            std::string sql =
                "SELECT user_id, session, COUNT(*) FROM turns "
                "WHERE session LIKE ?";
            if (user_id >= 0) sql += " AND user_id = ?";
            sql += " GROUP BY user_id, session";
            Stmt s(db_, sql.c_str());
            s.bind_text(1, std::string(CRON_SESSION_PREFIX) + "%");
            if (user_id >= 0) s.bind_int64(2, user_id);
            while (s.step())
                sessions.push_back({s.col_int64(0), s.col_text(1), s.col_int64(2)});
        }
        {
            // source='auto' as well as the preamble: an explicit `remember`
            // made *by* a scheduled job is a fact the job deliberately stored,
            // not the transcript-echo this is here to remove. Only the echo
            // quotes the preamble back, and only auto-memories are echoes.
            std::string sql =
                "SELECT user_id, id FROM memories "
                "WHERE source = 'auto' AND text LIKE ?";
            if (user_id >= 0) sql += " AND user_id = ?";
            Stmt s(db_, sql.c_str());
            s.bind_text(1, std::string("User said: \"") + CRON_TASK_PREAMBLE + "%");
            if (user_id >= 0) s.bind_int64(2, user_id);
            while (s.step()) memories.emplace_back(s.col_int64(0), s.col_int64(1));
        }
    }

    // Sessions are counted either way — a dry run should report what is
    // there, and the caller decides separately whether to act on it.
    for (const auto& t : sessions) {
        report.sessions += 1;
        report.turns    += t.turns;
        if (!opt.dry_run && opt.drop_sessions) delete_session(t.user_id, t.session);
    }
    for (const auto& [uid, id] : memories) {
        report.memories += 1;
        if (!opt.dry_run) forget(uid, id);
    }
    return report;
}

int64_t MemoryStore::delete_session(int64_t user_id, const std::string& session) {
    if (user_id <= 0 || session.empty()) return 0;
    std::lock_guard<std::mutex> lock(mu_);

    // One transaction: a conversation half-deleted — turns gone, summary left
    // behind — would come back as an empty session carrying the summary of
    // what the user asked to erase.
    exec_or_throw(db_, "BEGIN IMMEDIATE;");
    int64_t removed = 0;
    try {
        // user_id sits on the DELETE itself rather than in a check before it,
        // the same as forget(): a check-then-act is a race, and this way a
        // session name belonging to another account simply matches no rows.
        {
            Stmt s(db_, "DELETE FROM turns WHERE user_id = ? AND session = ?");
            s.bind_int64(1, user_id);
            s.bind_text(2, session);
            s.step();
            removed = sqlite3_changes(db_);
        }
        {
            Stmt s(db_, "DELETE FROM session_summaries WHERE user_id = ? AND session = ?");
            s.bind_int64(1, user_id);
            s.bind_text(2, session);
            s.step();
        }
        {
            Stmt s(db_, "DELETE FROM tool_results WHERE user_id = ? AND session = ?");
            s.bind_int64(1, user_id);
            s.bind_text(2, session);
            s.step();
        }
        exec_or_throw(db_, "COMMIT;");
    } catch (...) {
        exec_or_throw(db_, "ROLLBACK;");
        throw;
    }
    return removed;
}

// ── rolling summary ───────────────────────────────────────────────────────────

std::string MemoryStore::get_summary(int64_t user_id, const std::string& session) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT summary FROM session_summaries WHERE session = ? AND user_id = ?");
    s.bind_text(1, session);
    s.bind_int64(2, user_id);
    return s.step() ? s.col_text(0) : "";
}

void MemoryStore::set_summary(int64_t user_id, const std::string& session,
                              const std::string& agent, const std::string& summary) {
    std::lock_guard<std::mutex> lock(mu_);
    // The conflict target is (user_id, session), the table's primary key since
    // the rebuild in migrate(). Two accounts using the same session id now get
    // a row each, so no guard on the UPDATE arm is needed — before the rebuild
    // the target was `session` alone and the second account silently stored
    // nothing at all.
    Stmt s(db_, R"sql(
        INSERT INTO session_summaries(session, user_id, agent, summary, updated_at)
        VALUES (?, ?, ?, ?, datetime('now'))
        ON CONFLICT(user_id, session) DO UPDATE SET
            summary = excluded.summary, updated_at = excluded.updated_at
    )sql");
    s.bind_text(1, session);
    s.bind_int64(2, user_id);
    s.bind_text(3, agent);
    s.bind_text(4, summary);
    s.step();
}

// ── sessions ───────────────────────────────────────────────────────────────────

std::vector<MemoryStore::SessionSummary> MemoryStore::list_sessions(int64_t user_id,
                                                                    int limit,
                                                                    bool include_cron) {
    std::lock_guard<std::mutex> lock(mu_);

    // The filter goes inside the grouped subquery, not outside it: applied
    // after the LIMIT it would return short pages on a database where the
    // newest sessions are all scheduled runs, which on a busy scheduler is
    // most of them.
    const std::string cron_filter = include_cron
        ? std::string()
        : std::string(" AND session NOT LIKE '") + CRON_SESSION_PREFIX + "%'";

    Stmt s(db_, (std::string(R"sql(
        SELECT g.session, g.last_at, g.cnt,
               (SELECT content FROM turns
                WHERE session = g.session AND user_id = ? AND role = 'user'
                ORDER BY id ASC LIMIT 1) AS preview
        FROM (
            SELECT session, MAX(created_at) AS last_at, MAX(id) AS last_id, COUNT(*) AS cnt
            FROM turns
            WHERE user_id = ? AND role IN ('user', 'assistant'))sql")
        + cron_filter + R"sql(
            GROUP BY session
        ) g
        ORDER BY g.last_at DESC, g.last_id DESC
        LIMIT ?
    )sql").c_str());
    s.bind_int64(1, user_id);
    s.bind_int64(2, user_id);
    s.bind_int64(3, limit);

    constexpr size_t kMaxPreviewBytes = 120;
    std::vector<SessionSummary> out;
    while (s.step()) {
        SessionSummary summary;
        summary.session         = s.col_text(0);
        summary.last_message_at = s.col_text(1);
        summary.turn_count      = s.col_int64(2);
        summary.preview         = s.col_text(3);
        if (summary.preview.size() > kMaxPreviewBytes) {
            funes::truncate_utf8_safe(summary.preview, kMaxPreviewBytes);
            summary.preview += "…";
        }
        out.push_back(std::move(summary));
    }
    return out;
}

// ── cron jobs ──────────────────────────────────────────────────────────────────

namespace {

MemoryStore::CronJob cron_job_from_row(Stmt& s) {
    MemoryStore::CronJob job;
    job.id          = s.col_int64(0);
    job.name        = s.col_text(1);
    job.kind        = s.col_text(2);
    job.agent       = s.col_text(3);
    job.task        = s.col_text(4);
    job.command     = s.col_text(5);
    job.schedule    = s.col_text(6);
    job.running     = s.col_int64(7) != 0;
    job.next_run_at = s.col_int64(8);
    job.last_run_at = s.col_int64(9);
    job.last_status = s.col_text(10);
    job.last_output = s.col_text(11);
    job.user_id     = s.col_int64(12);
    return job;
}

constexpr const char* kCronJobColumns =
    "id, name, kind, agent, task, command, schedule, running, "
    "next_run_at, last_run_at, last_status, last_output, user_id";

} // namespace

int64_t MemoryStore::create_cron_job(const CronJob& job) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, R"sql(
        INSERT INTO cron_jobs(name, kind, agent, task, command, schedule,
                              created_at, next_run_at, user_id)
        VALUES (?,?,?,?,?,?,strftime('%s','now'),?,?)
    )sql");
    s.bind_text(1, job.name);
    s.bind_text(2, job.kind);
    s.bind_text(3, job.agent);
    s.bind_text(4, job.task);
    s.bind_text(5, job.command);
    s.bind_text(6, job.schedule);
    s.bind_int64(7, job.next_run_at);
    s.bind_int64(8, job.user_id);
    s.step();
    return sqlite3_last_insert_rowid(db_);
}

std::vector<MemoryStore::CronJob> MemoryStore::list_cron_jobs(int64_t user_id) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string sql = std::string("SELECT ") + kCronJobColumns + " FROM cron_jobs";
    if (user_id >= 0) sql += " WHERE user_id = ?";
    sql += " ORDER BY id DESC";
    Stmt s(db_, sql.c_str());
    if (user_id >= 0) s.bind_int64(1, user_id);
    std::vector<CronJob> out;
    while (s.step()) out.push_back(cron_job_from_row(s));
    return out;
}

bool MemoryStore::delete_cron_job(int64_t user_id, int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    // user_id < 0 is the admin path (cancel any job). Otherwise the owner
    // predicate is on the DELETE, so cancelling someone else's scheduled job
    // is indistinguishable from cancelling one that doesn't exist.
    std::string sql = "DELETE FROM cron_jobs WHERE id = ?";
    if (user_id >= 0) sql += " AND user_id = ?";
    Stmt s(db_, sql.c_str());
    s.bind_int64(1, id);
    if (user_id >= 0) s.bind_int64(2, user_id);
    s.step();
    return sqlite3_changes(db_) > 0;
}

std::vector<MemoryStore::CronJob> MemoryStore::due_cron_jobs(int64_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, (std::string("SELECT ") + kCronJobColumns +
                " FROM cron_jobs WHERE running = 0 "
                "AND next_run_at <= ? ORDER BY next_run_at").c_str());
    s.bind_int64(1, now);
    std::vector<CronJob> out;
    while (s.step()) out.push_back(cron_job_from_row(s));
    return out;
}

std::optional<MemoryStore::CronJob> MemoryStore::get_cron_job(int64_t user_id, int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    std::string sql = std::string("SELECT ") + kCronJobColumns +
                      " FROM cron_jobs WHERE id = ?";
    if (user_id >= 0) sql += " AND user_id = ?";
    Stmt s(db_, sql.c_str());
    s.bind_int64(1, id);
    if (user_id >= 0) s.bind_int64(2, user_id);
    if (!s.step()) return std::nullopt;
    return cron_job_from_row(s);
}

void MemoryStore::mark_cron_job_running(int64_t id, bool running) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "UPDATE cron_jobs SET running = ? WHERE id = ?");
    s.bind_int64(1, running ? 1 : 0);
    s.bind_int64(2, id);
    s.step();
}

void MemoryStore::record_cron_job_run(int64_t id, bool ok, const std::string& output_preview,
                                      int64_t next_run_at) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, R"sql(
        UPDATE cron_jobs SET
            last_run_at = strftime('%s','now'),
            last_status = ?,
            last_output = ?,
            next_run_at = ?
        WHERE id = ?
    )sql");
    s.bind_text(1, ok ? "ok" : "error");
    s.bind_text(2, output_preview);
    s.bind_int64(3, next_run_at);
    s.bind_int64(4, id);
    s.step();
}
