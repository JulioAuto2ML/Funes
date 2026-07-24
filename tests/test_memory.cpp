// =============================================================================
// tests/test_memory.cpp — MemoryStore unit tests
// =============================================================================
// Covers: keyword-only mode (no embedder), semantic mode (fake deterministic
// embedder), dedup, forget, turns, backfill.

#include "memory.h"
#include <algorithm>
#include <cassert>
#include <cmath>
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

// Deterministic fake embedder: normalized letter histogram (26 dims).
// Texts sharing words get high cosine similarity; identical text → 1.0.
class FakeEmbedder : public EmbeddingClient {
public:
    FakeEmbedder() : EmbeddingClient("http://unused:1", "", "fake") {}
    bool fail = false;

    std::vector<float> embed(const std::string& text) override {
        if (fail) throw std::runtime_error("fake embedder down");
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
    fs::path p = fs::temp_directory_path() / (std::string("funes_test_") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

int test_keyword_only() {
    MemoryStore store(temp_db("kw"), nullptr);
    CHECK(!store.semantic_available());

    int64_t id1 = store.remember("funes", "The user's favorite color is blue", "user");
    int64_t id2 = store.remember("funes", "The user lives in Buenos Aires", "auto");
    CHECK(id1 > 0 && id2 > 0 && id1 != id2);

    // Dedup: same (agent, text) returns the same id.
    CHECK(store.remember("funes", "The user's favorite color is blue", "user") == id1);
    CHECK(store.count("funes") == 2);
    CHECK(store.count() == 2);

    // Keyword recall.
    auto r = store.recall("funes", "favorite color", 5);
    CHECK(r.size() == 1);
    CHECK(r[0].id == id1);
    CHECK(r[0].source == "user");

    // Agent scoping.
    store.remember("other", "Something about color theory", "user");
    CHECK(store.recall("funes", "color", 5).size() == 1);
    CHECK(store.recall("", "color", 5).size() == 2);

    // LIKE wildcards in the query must not act as wildcards.
    CHECK(store.recall("funes", "%", 5).empty());

    // list is newest-first.
    auto all = store.list("funes");
    CHECK(all.size() == 2);
    CHECK(all[0].id == id2);

    // forget.
    CHECK(store.forget(id1));
    CHECK(!store.forget(id1));
    CHECK(store.count("funes") == 1);
    return 0;
}

int test_semantic() {
    FakeEmbedder emb;
    MemoryStore store(temp_db("sem"), &emb);

    store.remember("funes", "zzzz qqqq jjjj", "user");                  // letter-distant
    int64_t id = store.remember("funes", "the cat sat on the mat", "user");

    auto r = store.recall("funes", "the cat sat on the mat", 2);
    CHECK(!r.empty());
    CHECK(r[0].id == id);            // exact text ranks first
    CHECK(r[0].score > 0.99);        // cosine similarity ≈ 1

    // Embedder failure mid-flight → keyword fallback, no crash.
    emb.fail = true;
    auto r2 = store.recall("funes", "cat", 5);
    CHECK(!r2.empty());
    CHECK(!store.semantic_available());

    // Memory stored while the embedder is down has no vector...
    int64_t id3 = store.remember("funes", "dogs bark loudly", "user");
    CHECK(id3 > 0);

    // ...until backfill runs after the embedder recovers.
    emb.fail = false;
    CHECK(store.backfill_embeddings() >= 1);
    auto r3 = store.recall("funes", "dogs bark loudly", 1);
    CHECK(!r3.empty());
    CHECK(r3[0].id == id3);
    return 0;
}

int test_turns() {
    MemoryStore store(temp_db("turns"), nullptr);

    store.append_turn("s1", "funes", "user", "hello");
    store.append_turn("s1", "funes", "assistant", "hi there");
    store.append_turn("s2", "funes", "user", "other session");

    auto turns = store.recent_turns("s1", 10);
    CHECK(turns.size() == 2);
    CHECK(turns[0].role == "user" && turns[0].content == "hello");
    CHECK(turns[1].role == "assistant");

    // Limit keeps the LAST n turns, in chronological order.
    store.append_turn("s1", "funes", "user", "second question");
    auto limited = store.recent_turns("s1", 2);
    CHECK(limited.size() == 2);
    CHECK(limited[1].content == "second question");
    return 0;
}

int test_list_sessions() {
    MemoryStore store(temp_db("sessions"), nullptr);

    // s1: two full exchanges, s2: one, created after s1 so it sorts first.
    store.append_turn("s1", "funes", "user", "what's the weather like");
    store.append_turn("s1", "funes", "assistant", "sunny");
    store.append_turn("s1", "funes", "user", "and tomorrow");
    store.append_turn("s1", "funes", "assistant", "rain");
    store.append_turn("s2", "operator", "user", "check disk space");
    store.append_turn("s2", "operator", "assistant", "42GB free");

    auto sessions = store.list_sessions();
    CHECK(sessions.size() == 2);

    // Most recently active session first (s2, created after s1's turns).
    CHECK(sessions[0].session == "s2");
    CHECK(sessions[0].turn_count == 2);
    CHECK(sessions[0].preview == "check disk space");

    CHECK(sessions[1].session == "s1");
    CHECK(sessions[1].turn_count == 4);
    // Preview is the FIRST user turn, not the latest.
    CHECK(sessions[1].preview == "what's the weather like");

    // A long first message gets truncated with an ellipsis.
    store.append_turn("s3", "funes", "user", std::string(200, 'x'));
    store.append_turn("s3", "funes", "assistant", "ok");
    auto with_long = store.list_sessions();
    auto it = std::find_if(with_long.begin(), with_long.end(),
                           [](const auto& s) { return s.session == "s3"; });
    CHECK(it != with_long.end());
    CHECK(it->preview.size() < 200);
    CHECK(it->preview.find("…") != std::string::npos);

    // limit is respected.
    CHECK(store.list_sessions(1).size() == 1);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_keyword_only();
    rc |= test_semantic();
    rc |= test_turns();
    rc |= test_list_sessions();
    if (rc == 0) std::cout << "test_memory: all tests passed\n";
    return rc;
}
