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

// A "tool" (deliberately taught) memory should outrank an "auto" (passive
// conversation log) one at equal raw cosine similarity — the NOOA-inspired
// gap this closes: a handful of auto-logged near-duplicates crowding a
// single authoritative fact out of a small top-k window. FakeEmbedder is a
// bag-of-letters histogram, so two texts using the same words in a
// different order embed identically — a reliable way to force an exact
// similarity tie without relying on real embedding-model behavior.
int test_source_weighted_ranking() {
    FakeEmbedder emb;
    MemoryStore store(temp_db("srcweight"), &emb);

    int64_t id_auto = store.remember("funes", "the cat sat on the mat", "auto");
    int64_t id_tool = store.remember("funes", "mat the cat on sat the", "tool");

    auto r = store.recall("funes", "the cat sat on the mat", 2);
    CHECK(r.size() == 2);
    CHECK(r[0].id == id_tool);       // tool's weight breaks an exact raw tie
    CHECK(r[1].id == id_auto);
    CHECK(r[0].score > r[1].score);

    // Raw similarities are equal by construction (same letters, different
    // word order — see the comment above), so the score gap is exactly the
    // ratio between MemoryStore::source_weight("tool") and ("auto"): 1.3.
    CHECK(std::abs(r[0].score / r[1].score - 1.3) < 1e-6);
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

// ── consolidation ─────────────────────────────────────────────────────────────

int test_recall_count() {
    MemoryStore store(temp_db("recall_count"), nullptr);
    int64_t id = store.remember("funes", "the user prefers dark roast coffee", "auto");

    CHECK(store.list("funes")[0].recall_count == 0);

    store.recall("funes", "coffee", 5);
    CHECK(store.list("funes")[0].recall_count == 1);
    store.recall("funes", "coffee", 5);
    CHECK(store.list("funes")[0].recall_count == 2);

    // Browsing (touch=false) is not a recall: it must not shield a memory
    // from the prune below.
    store.recall("funes", "coffee", 5, /*touch=*/false);
    CHECK(store.list("funes")[0].recall_count == 2);

    // A miss touches nothing.
    store.recall("funes", "nothing matches this", 5);
    CHECK(store.list("funes")[0].recall_count == 2);
    CHECK(id > 0);
    return 0;
}

int test_prune_respects_source_age_and_recall() {
    MemoryStore store(temp_db("prune"), nullptr);   // keyword-only: merge step skipped

    store.remember("funes", "explicitly remembered fact", "user");
    store.remember("funes", "auto captured and never used", "auto");
    store.remember("funes", "auto captured but recalled later", "auto");
    store.recall("funes", "recalled later", 5);     // → recall_count 1

    bool merge_called = false;
    auto merge = [&](const std::vector<std::string>&) {
        merge_called = true;
        return std::string("merged");
    };

    // Nothing is old enough yet.
    MemoryStore::ConsolidationOptions young;
    young.prune_after_days = 1;
    CHECK(store.consolidate(merge, young).pruned == 0);
    CHECK(store.count("funes") == 3);

    // Age floor removed: only the auto memory nobody ever recalled goes.
    MemoryStore::ConsolidationOptions now;
    now.prune_after_days = 0;
    auto report = store.consolidate(merge, now);
    CHECK(report.pruned == 1);
    CHECK(store.count("funes") == 2);

    auto left = store.list("funes");
    for (const auto& m : left)
        CHECK(m.text != "auto captured and never used");

    // Without an embedder there is nothing to cluster on, so the LLM is never
    // called — a keyword-only install still gets the prune, for free.
    CHECK(!merge_called);
    CHECK(report.clusters_seen == 0);

    // Negative = pruning off entirely.
    MemoryStore::ConsolidationOptions off;
    off.prune_after_days = -1;
    store.remember("funes", "another never-recalled auto memory", "auto");
    CHECK(store.consolidate(merge, off).pruned == 0);
    CHECK(store.count("funes") == 3);
    return 0;
}

int test_merge_near_duplicates() {
    FakeEmbedder emb;
    MemoryStore store(temp_db("merge"), &emb);

    // Same letters → identical vector under FakeEmbedder → cosine 1.0. Two
    // rows saying the same thing in different words is exactly the case.
    store.remember("funes", "the cat sat on the mat", "auto");
    store.remember("funes", "the mat sat on the cat", "auto");
    store.remember("funes", "zzzz qqqq jjjj", "auto");   // distant, untouched

    MemoryStore::ConsolidationOptions opt;
    opt.prune_after_days = -1;   // isolate the merge step

    int calls = 0;
    size_t cluster_size = 0;
    auto report = store.consolidate([&](const std::vector<std::string>& texts) {
        ++calls;
        cluster_size = texts.size();
        return std::string("A cat and a mat sat on each other");
    }, opt);

    CHECK(calls == 1);
    CHECK(cluster_size == 2);
    CHECK(report.clusters_seen == 1);
    CHECK(report.merged == 2);
    CHECK(store.count("funes") == 2);           // 3 − 2 originals + 1 merged

    bool found = false;
    for (const auto& m : store.list("funes")) {
        if (m.text == "A cat and a mat sat on each other") {
            found = true;
            CHECK(m.source == "consolidated");
        }
    }
    CHECK(found);

    // The merged row is searchable, i.e. it got its own embedding.
    auto r = store.recall("funes", "A cat and a mat sat on each other", 1);
    CHECK(!r.empty());
    CHECK(r[0].source == "consolidated");
    return 0;
}

int test_merge_keep_all_and_failure() {
    FakeEmbedder emb;
    MemoryStore store(temp_db("keepall"), &emb);

    store.remember("funes", "the cat sat on the mat", "auto");
    store.remember("funes", "the mat sat on the cat", "auto");

    MemoryStore::ConsolidationOptions opt;
    opt.prune_after_days = -1;

    // "KEEP ALL" means they only looked alike — nothing is touched.
    auto kept = store.consolidate(
        [](const std::vector<std::string>&) { return std::string("KEEP ALL"); }, opt);
    CHECK(kept.clusters_seen == 1);
    CHECK(kept.kept == 1);
    CHECK(kept.merged == 0);
    CHECK(store.count("funes") == 2);

    // A merge call that throws (LLM down mid-run) must leave the cluster
    // exactly as it was: no half-applied merge, no lost memory.
    auto failed = store.consolidate(
        [](const std::vector<std::string>&) -> std::string {
            throw std::runtime_error("LLM unavailable");
        }, opt);
    CHECK(failed.merged == 0);
    CHECK(store.count("funes") == 2);
    CHECK(store.recall("funes", "the cat sat on the mat", 1).size() == 1);

    // And the run after recovery still works — nothing was left claimed or
    // locked by the failure.
    auto ok = store.consolidate(
        [](const std::vector<std::string>&) { return std::string("cats and mats, together"); }, opt);
    CHECK(ok.merged == 2);
    CHECK(store.count("funes") == 1);
    return 0;
}

int test_consolidation_scoping() {
    FakeEmbedder emb;
    MemoryStore store(temp_db("scope"), &emb);

    // Identical text under two agents is not a duplicate: the same sentence
    // means different things in two agents' histories.
    store.remember("funes", "the cat sat on the mat", "auto");
    store.remember("operator", "the mat sat on the cat", "auto");

    MemoryStore::ConsolidationOptions opt;
    opt.prune_after_days = -1;
    auto report = store.consolidate(
        [](const std::vector<std::string>&) { return std::string("merged across agents"); }, opt);
    CHECK(report.clusters_seen == 0);
    CHECK(store.count() == 2);

    // max_clusters caps the LLM calls per run; the next run continues.
    store.remember("funes", "the mat sat on the cat", "auto");
    store.remember("funes", "alpha beta gamma", "auto");
    store.remember("funes", "gamma beta alpha", "auto");
    opt.max_clusters = 1;
    int calls = 0;
    store.consolidate([&](const std::vector<std::string>&) {
        ++calls;
        return std::string("merged " + std::to_string(calls));
    }, opt);
    CHECK(calls == 1);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_keyword_only();
    rc |= test_semantic();
    rc |= test_source_weighted_ranking();
    rc |= test_turns();
    rc |= test_list_sessions();
    rc |= test_recall_count();
    rc |= test_prune_respects_source_age_and_recall();
    rc |= test_merge_near_duplicates();
    rc |= test_merge_keep_all_and_failure();
    rc |= test_consolidation_scoping();
    if (rc == 0) std::cout << "test_memory: all tests passed\n";
    return rc;
}
