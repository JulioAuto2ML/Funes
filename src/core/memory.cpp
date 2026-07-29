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
#include <cstring>
#include <iostream>
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
};

void exec_or_throw(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown sqlite error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + msg + " — SQL: " + sql);
    }
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
    )sql");

    // Restore the vec table dimension recorded by a previous run.
    Stmt s(db_, "SELECT value FROM meta WHERE key='embed_dim'");
    if (s.step())
        dim_ = std::stoi(s.col_text(0));
}

void MemoryStore::ensure_vec_table(int dim) {
    if (dim_ == dim) return;

    if (dim_ != 0) {
        std::cerr << "[memory] embedding dimension changed " << dim_ << " → " << dim
                  << "; rebuilding vector index (memory texts are preserved)\n";
        exec_or_throw(db_, "DROP TABLE IF EXISTS vec_memories;");
    }

    const std::string ddl =
        "CREATE VIRTUAL TABLE IF NOT EXISTS vec_memories USING vec0("
        "  memory_id INTEGER PRIMARY KEY,"
        "  embedding float[" + std::to_string(dim) + "] distance_metric=cosine"
        ");";
    exec_or_throw(db_, ddl.c_str());

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

void MemoryStore::insert_vector(int64_t memory_id, const std::vector<float>& vec) {
    ensure_vec_table(static_cast<int>(vec.size()));
    Stmt s(db_, "INSERT OR REPLACE INTO vec_memories(memory_id, embedding) VALUES(?, ?)");
    s.bind_int64(1, memory_id);
    s.bind_blob(2, vec.data(), vec.size() * sizeof(float));
    s.step();
}

// ── long-term memory ──────────────────────────────────────────────────────────

int64_t MemoryStore::remember(const std::string& agent, const std::string& text,
                              const std::string& source) {
    if (agent.empty() || text.empty())
        throw std::runtime_error("remember: agent and text must be non-empty");

    std::vector<float> vec;
    const bool have_vec = try_embed(text, vec);  // network call outside the lock

    std::lock_guard<std::mutex> lock(mu_);

    {
        Stmt s(db_, "INSERT OR IGNORE INTO memories(agent, text, source) VALUES(?,?,?)");
        s.bind_text(1, agent);
        s.bind_text(2, text);
        s.bind_text(3, source);
        s.step();
    }

    int64_t id = sqlite3_last_insert_rowid(db_);
    if (sqlite3_changes(db_) == 0) {
        // Duplicate (agent, text): fetch the existing row's id.
        Stmt s(db_, "SELECT id FROM memories WHERE agent=? AND text=?");
        s.bind_text(1, agent);
        s.bind_text(2, text);
        if (s.step()) id = s.col_int64(0);
        return id;  // vector already present (or backfill will handle it)
    }

    if (have_vec) {
        try { insert_vector(id, vec); }
        catch (const std::exception& e) {
            std::cerr << "[memory] vector insert failed for memory " << id
                      << ": " << e.what() << "\n";
        }
    }
    return id;
}

std::vector<MemoryStore::Memory> MemoryStore::recall(const std::string& agent,
                                                     const std::string& query, int k) {
    if (query.empty() || k <= 0) return {};

    std::vector<float> qvec;
    if (try_embed(query, qvec)) {
        std::lock_guard<std::mutex> lock(mu_);
        if (dim_ == static_cast<int>(qvec.size())) {
            auto results = recall_semantic(agent, qvec, k);
            if (!results.empty()) return results;
        }
        // No vec table yet, dimension mismatch, or empty index → keyword.
        return recall_keyword(agent, query, k);
    }

    std::lock_guard<std::mutex> lock(mu_);
    return recall_keyword(agent, query, k);
}

std::vector<MemoryStore::Memory> MemoryStore::recall_semantic(
    const std::string& agent, const std::vector<float>& qvec, int k) {
    // KNN over all agents, then filter — vec0 MATCH cannot combine with
    // arbitrary WHERE clauses. Over-fetch to survive the filter.
    const int fetch_k = agent.empty() ? k : k * 8;

    Stmt s(db_, R"sql(
        SELECT m.id, m.agent, m.text, m.source, m.created_at, v.distance
        FROM vec_memories v
        JOIN memories m ON m.id = v.memory_id
        WHERE v.embedding MATCH ? AND v.k = ?
        ORDER BY v.distance
    )sql");
    s.bind_blob(1, qvec.data(), qvec.size() * sizeof(float));
    s.bind_int64(2, fetch_k);

    std::vector<Memory> out;
    while (s.step()) {
        if (!agent.empty() && s.col_text(1) != agent) continue;
        Memory m;
        m.id         = s.col_int64(0);
        m.agent      = s.col_text(1);
        m.text       = s.col_text(2);
        m.source     = s.col_text(3);
        m.created_at = s.col_text(4);
        m.score      = 1.0 - s.col_double(5);  // cosine distance → similarity
        out.push_back(std::move(m));
        if (static_cast<int>(out.size()) >= k) break;
    }
    return out;
}

std::vector<MemoryStore::Memory> MemoryStore::recall_keyword(
    const std::string& agent, const std::string& query, int k) {
    std::string sql =
        "SELECT id, agent, text, source, created_at FROM memories "
        "WHERE text LIKE ? ESCAPE '\\'";
    if (!agent.empty()) sql += " AND agent = ?";
    sql += " ORDER BY id DESC LIMIT ?";

    Stmt s(db_, sql.c_str());
    int i = 1;
    s.bind_text(i++, like_pattern(query));
    if (!agent.empty()) s.bind_text(i++, agent);
    s.bind_int64(i, k);

    std::vector<Memory> out;
    while (s.step()) {
        Memory m;
        m.id         = s.col_int64(0);
        m.agent      = s.col_text(1);
        m.text       = s.col_text(2);
        m.source     = s.col_text(3);
        m.created_at = s.col_text(4);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<MemoryStore::Memory> MemoryStore::list(const std::string& agent,
                                                   int limit, int offset) {
    std::lock_guard<std::mutex> lock(mu_);

    std::string sql = "SELECT id, agent, text, source, created_at FROM memories";
    if (!agent.empty()) sql += " WHERE agent = ?";
    sql += " ORDER BY id DESC LIMIT ? OFFSET ?";

    Stmt s(db_, sql.c_str());
    int i = 1;
    if (!agent.empty()) s.bind_text(i++, agent);
    s.bind_int64(i++, limit);
    s.bind_int64(i, offset);

    std::vector<Memory> out;
    while (s.step()) {
        Memory m;
        m.id         = s.col_int64(0);
        m.agent      = s.col_text(1);
        m.text       = s.col_text(2);
        m.source     = s.col_text(3);
        m.created_at = s.col_text(4);
        out.push_back(std::move(m));
    }
    return out;
}

bool MemoryStore::forget(int64_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (dim_ != 0) {
        Stmt v(db_, "DELETE FROM vec_memories WHERE memory_id = ?");
        v.bind_int64(1, id);
        v.step();
    }
    Stmt s(db_, "DELETE FROM memories WHERE id = ?");
    s.bind_int64(1, id);
    s.step();
    return sqlite3_changes(db_) > 0;
}

int64_t MemoryStore::count(const std::string& agent) {
    std::lock_guard<std::mutex> lock(mu_);
    if (agent.empty()) {
        Stmt s(db_, "SELECT COUNT(*) FROM memories");
        s.step();
        return s.col_int64(0);
    }
    Stmt s(db_, "SELECT COUNT(*) FROM memories WHERE agent = ?");
    s.bind_text(1, agent);
    s.step();
    return s.col_int64(0);
}

size_t MemoryStore::backfill_embeddings(size_t max_items) {
    if (!embedder_) return 0;

    // Collect candidates under the lock, embed outside it.
    std::vector<std::pair<int64_t, std::string>> missing;
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::string sql = (dim_ == 0)
            ? "SELECT id, text FROM memories ORDER BY id LIMIT ?"
            : "SELECT m.id, m.text FROM memories m "
              "LEFT JOIN vec_memories v ON v.memory_id = m.id "
              "WHERE v.memory_id IS NULL ORDER BY m.id LIMIT ?";
        Stmt s(db_, sql.c_str());
        s.bind_int64(1, static_cast<int64_t>(max_items));
        while (s.step())
            missing.emplace_back(s.col_int64(0), s.col_text(1));
    }

    size_t done = 0;
    for (const auto& [id, text] : missing) {
        std::vector<float> vec;
        if (!try_embed(text, vec)) break;  // endpoint down — stop, retry later
        std::lock_guard<std::mutex> lock(mu_);
        try {
            insert_vector(id, vec);
            ++done;
        } catch (const std::exception& e) {
            std::cerr << "[memory] backfill failed for memory " << id
                      << ": " << e.what() << "\n";
            break;
        }
    }
    return done;
}

// ── conversation history ──────────────────────────────────────────────────────

void MemoryStore::append_turn(const std::string& session, const std::string& agent,
                              const std::string& role, const std::string& content) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "INSERT INTO turns(session, agent, role, content) VALUES(?,?,?,?)");
    s.bind_text(1, session);
    s.bind_text(2, agent);
    s.bind_text(3, role);
    s.bind_text(4, content);
    s.step();
}

std::vector<ChatMessage> MemoryStore::recent_turns(const std::string& session, int n) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, R"sql(
        SELECT role, content FROM (
            SELECT id, role, content FROM turns
            WHERE session = ? AND role IN ('user','assistant')
            ORDER BY id DESC LIMIT ?
        ) ORDER BY id ASC
    )sql");
    s.bind_text(1, session);
    s.bind_int64(2, n);

    std::vector<ChatMessage> out;
    while (s.step()) {
        ChatMessage m;
        m.role    = s.col_text(0);
        m.content = s.col_text(1);
        out.push_back(std::move(m));
    }
    return out;
}

int64_t MemoryStore::turn_count(const std::string& session) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT COUNT(*) FROM turns WHERE session = ? AND role IN ('user','assistant')");
    s.bind_text(1, session);
    s.step();
    return s.col_int64(0);
}

void MemoryStore::prune_turns(const std::string& session, int keep) {
    if (keep < 0) keep = 0;
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, R"sql(
        DELETE FROM turns WHERE session = ? AND id NOT IN (
            SELECT id FROM turns WHERE session = ? ORDER BY id DESC LIMIT ?
        )
    )sql");
    s.bind_text(1, session);
    s.bind_text(2, session);
    s.bind_int64(3, keep);
    s.step();
}

// ── rolling summary ───────────────────────────────────────────────────────────

std::string MemoryStore::get_summary(const std::string& session) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, "SELECT summary FROM session_summaries WHERE session = ?");
    s.bind_text(1, session);
    return s.step() ? s.col_text(0) : "";
}

void MemoryStore::set_summary(const std::string& session, const std::string& agent,
                              const std::string& summary) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt s(db_, R"sql(
        INSERT INTO session_summaries(session, agent, summary, updated_at)
        VALUES (?, ?, ?, datetime('now'))
        ON CONFLICT(session) DO UPDATE SET
            summary = excluded.summary, updated_at = excluded.updated_at
    )sql");
    s.bind_text(1, session);
    s.bind_text(2, agent);
    s.bind_text(3, summary);
    s.step();
}

// ── sessions ───────────────────────────────────────────────────────────────────

std::vector<MemoryStore::SessionSummary> MemoryStore::list_sessions(int limit) {
    std::lock_guard<std::mutex> lock(mu_);

    Stmt s(db_, R"sql(
        SELECT g.session, g.last_at, g.cnt,
               (SELECT content FROM turns
                WHERE session = g.session AND role = 'user'
                ORDER BY id ASC LIMIT 1) AS preview
        FROM (
            SELECT session, MAX(created_at) AS last_at, MAX(id) AS last_id, COUNT(*) AS cnt
            FROM turns
            WHERE role IN ('user', 'assistant')
            GROUP BY session
        ) g
        ORDER BY g.last_at DESC, g.last_id DESC
        LIMIT ?
    )sql");
    s.bind_int64(1, limit);

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
