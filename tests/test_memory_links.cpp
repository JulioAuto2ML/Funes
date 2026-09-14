// =============================================================================
// tests/test_memory_links.cpp — connected memories (5.0)
// =============================================================================
// Recall before 5.0 was a flat similarity search: a fact was retrievable only
// if the query looked like it. Two facts about the same thing, worded
// differently, never reinforced each other — ask about one and the other stays
// invisible, however obviously related a human would find them.
//
// 5.0 adds two ways for a memory to be reached that don't depend on wording:
// an explicit link to a memory that *was* matched, and an FTS5 term match.
// Both are additive, and the assertions below are mostly about what they must
// NOT do — because the risk in widening recall is not that it fails, it is
// that it quietly returns worse results than the flat search it replaced, and
// nobody can tell.
//
// The three properties that matter:
//   * a link cannot cross accounts, in either direction;
//   * an expanded hit cannot outrank the hit it was expanded from;
//   * with no links and no term matches, recall returns exactly what it
//     returned before.

#include "memory.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace fs = std::filesystem;
using Memory = MemoryStore::Memory;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

// Letter-frequency embedder: deterministic cosine similarity, no model. Two
// texts are "similar" here exactly when they share letters, which is what lets
// a test say "unrelated" and mean it.
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
    fs::path p = fs::temp_directory_path() / (std::string("funes_links_") + name + ".db");
    fs::remove(p);
    fs::remove(p.string() + "-wal");
    fs::remove(p.string() + "-shm");
    return p.string();
}

// Cosine between two texts under the test embedder, so a test can state a
// similarity threshold in terms of the fixture instead of guessing one.
static double cosine(FakeEmbedder& e, const std::string& a, const std::string& b) {
    auto va = e.embed(a), vb = e.embed(b);
    double dot = 0;
    for (size_t i = 0; i < va.size(); ++i) dot += va[i] * vb[i];
    return dot;
}

static bool has_id(const std::vector<Memory>& hits, int64_t id) {
    for (const auto& m : hits) if (m.id == id) return true;
    return false;
}

static double score_of(const std::vector<Memory>& hits, int64_t id) {
    for (const auto& m : hits) if (m.id == id) return m.score;
    return -1.0;
}

int test_links_are_stored_and_scoped_to_one_account() {
    FakeEmbedder embedder;
    MemoryStore store(temp_db("scope"), &embedder);

    const int64_t mine  = store.remember(1, "funes", "zebra crossing repainted", "user");
    const int64_t also  = store.remember(1, "funes", "the council repaints crossings", "user");
    const int64_t yours = store.remember(2, "funes", "unrelated fact for user two", "user");

    CHECK(store.link_memories(1, mine, also, "related", 1.0));

    // Idempotent: the backfill pass will propose the same pair more than once
    // across runs, and a second proposal must not be an error or a duplicate.
    CHECK(store.link_memories(1, mine, also, "related", 1.0));
    CHECK(store.links_of(1, mine).size() == 1);

    // Both endpoints must belong to the caller. Refused in both directions:
    // "link my memory to theirs" and "link theirs to mine" are the same leak.
    CHECK(!store.link_memories(1, mine, yours, "related", 1.0));
    CHECK(!store.link_memories(1, yours, mine, "related", 1.0));
    CHECK(!store.link_memories(2, yours, mine, "related", 1.0));
    // And a link to something that does not exist at all.
    CHECK(!store.link_memories(1, mine, 999999, "related", 1.0));

    // Reading links is scoped too — asking as the wrong account sees nothing
    // rather than seeing the shape of somebody else's graph.
    CHECK(store.links_of(2, mine).empty());

    // Stored undirected for reading: the relation is symmetric to a human, so
    // asking either endpoint finds it.
    CHECK(store.links_of(1, also).size() == 1);
    CHECK(store.links_of(1, also)[0].other_id == mine);

    CHECK(store.unlink_memories(1, mine, also, "related"));
    CHECK(store.links_of(1, mine).empty());
    return 0;
}

int test_deleting_a_memory_takes_its_links() {
    FakeEmbedder embedder;
    MemoryStore store(temp_db("cascade"), &embedder);

    const int64_t a = store.remember(1, "funes", "alpha alpha alpha", "user");
    const int64_t b = store.remember(1, "funes", "beta beta beta", "user");
    CHECK(store.link_memories(1, a, b, "related", 1.0));

    CHECK(store.forget(1, a));
    // A dangling link would make recall expand into a memory that no longer
    // exists — at best a wasted join, at worst a row from a recycled id.
    CHECK(store.links_of(1, b).empty());
    return 0;
}

// Memories that are mildly similar to a "kayak" query (they share an 'a') but
// nowhere near it. Their job is to fill the top-k so that a zero-similarity
// memory is genuinely out of reach — without them the pool is smaller than k
// and recall returns everything, which would make the tests below pass for a
// reason that has nothing to do with links.
static void add_filler(MemoryStore& store) {
    store.remember(1, "funes", "cargo manifests updated", "auto");
    store.remember(1, "funes", "the harbour master called", "auto");
    store.remember(1, "funes", "mapping tables refreshed", "auto");
    store.remember(1, "funes", "standard rates apply", "auto");
}

int test_recall_reaches_a_linked_memory_it_could_not_match() {
    FakeEmbedder embedder;
    MemoryStore store(temp_db("expand"), &embedder);

    // "brief worlds shone" shares not one letter with the query, so flat
    // similarity search cannot get from one to the other at any k that matters.
    const int64_t anchor  = store.remember(1, "funes", "kayak kayak kayak", "user");
    const int64_t distant = store.remember(1, "funes", "brief worlds shone", "user");
    add_filler(store);

    auto before = store.recall(1, "funes", "kayak kayak kayak", 3);
    CHECK(has_id(before, anchor));
    CHECK(!has_id(before, distant));          // the point: invisible without a link

    CHECK(store.link_memories(1, anchor, distant, "related", 1.0));

    auto after = store.recall(1, "funes", "kayak kayak kayak", 3);
    CHECK(has_id(after, anchor));
    CHECK(has_id(after, distant));            // reached in one hop

    // One hop, not transitive: a chain would drag the whole graph into every
    // recall, and the far end of it is not what was asked about.
    const int64_t far = store.remember(1, "funes", "quixotic vug mynah", "user");
    CHECK(store.link_memories(1, distant, far, "related", 1.0));
    auto still = store.recall(1, "funes", "kayak kayak kayak", 4);
    CHECK(has_id(still, distant));
    CHECK(!has_id(still, far));
    return 0;
}

int test_an_expanded_hit_cannot_outrank_its_anchor() {
    FakeEmbedder embedder;
    MemoryStore store(temp_db("rank"), &embedder);

    const int64_t anchor = store.remember(1, "funes", "kayak kayak kayak", "user");
    const int64_t strong = store.remember(1, "funes", "brief worlds shone", "auto");
    const int64_t weak   = store.remember(1, "funes", "prudent old effort", "auto");
    add_filler(store);

    CHECK(store.link_memories(1, anchor, strong, "related", 1.0));
    CHECK(store.link_memories(1, anchor, weak,   "related", 0.5));

    auto hits = store.recall(1, "funes", "kayak kayak kayak", 5);
    CHECK(has_id(hits, anchor) && has_id(hits, strong) && has_id(hits, weak));

    // A memory reached *through* another is evidence about that other one, not
    // a better answer than it. If this inverts, a link becomes a way for a
    // weakly related memory to take the top slot from a direct match.
    CHECK(score_of(hits, strong) < score_of(hits, anchor));
    CHECK(hits[0].id == anchor);

    // And the link's weight is what separates them: same anchor, same distance
    // from the query, different strength of relation.
    CHECK(score_of(hits, weak) < score_of(hits, strong));
    CHECK(score_of(hits, weak) > 0.0);
    return 0;
}

int test_term_matches_fill_slots_without_displacing() {
    FakeEmbedder embedder;
    MemoryStore store(temp_db("fts"), &embedder);

    // A term match the vector search is bad at: the word is rare and the
    // surrounding letters drown it out.
    const int64_t exact = store.remember(1, "funes", "the quetzalcoatlus wingspan estimate", "user");
    for (int i = 0; i < 6; ++i)
        store.remember(1, "funes", "aeiou filler number " + std::to_string(i), "auto");

    auto hits = store.recall(1, "funes", "quetzalcoatlus", 5);
    CHECK(has_id(hits, exact));

    // Never across accounts: FTS5 has no user partition of its own, so the
    // join has to carry the predicate — this is the assertion that catches it
    // if it is ever dropped.
    store.remember(2, "funes", "quetzalcoatlus is user two's word", "user");
    auto mine = store.recall(1, "funes", "quetzalcoatlus", 5);
    for (const auto& m : mine) CHECK(m.user_id == 1);

    // And the agent filter still applies.
    store.remember(1, "other-agent", "quetzalcoatlus in another agent", "user");
    auto scoped = store.recall(1, "funes", "quetzalcoatlus", 5);
    for (const auto& m : scoped) CHECK(m.agent == "funes");
    return 0;
}

int test_recall_is_unchanged_when_there_is_nothing_to_add() {
    FakeEmbedder embedder;
    MemoryStore store(temp_db("regress"), &embedder);

    // Distinguishable to the letter-frequency embedder: texts that differ
    // only by a digit embed identically, which would make "best first" an
    // assertion about tie-breaking rather than about ranking.
    const char* texts[] = {"apples ripen early", "bananas ship green",
                           "cherries bruise fast", "dates keep dry",
                           "elderberries stain hands"};
    std::vector<int64_t> ids;
    for (const char* t : texts) ids.push_back(store.remember(1, "funes", t, "auto"));

    auto hits = store.recall(1, "funes", "cherries bruise fast", 3);
    CHECK(hits.size() == 3);                       // still k-bounded
    CHECK(hits[0].text == std::string("cherries bruise fast"));   // still best-first

    // No links exist, so nothing was expanded; every returned id is a direct
    // hit and the order is the source-weighted similarity order it always was.
    for (size_t i = 1; i < hits.size(); ++i)
        CHECK(hits[i - 1].score >= hits[i].score);

    // k is still a ceiling once links do exist.
    CHECK(store.link_memories(1, ids[2], ids[0], "related", 1.0));
    CHECK(store.link_memories(1, ids[2], ids[1], "related", 1.0));
    CHECK(store.link_memories(1, ids[2], ids[3], "related", 1.0));
    CHECK(store.recall(1, "funes", "cherries bruise fast", 3).size() == 3);

    // A memory reached twice — directly and through a link — appears once.
    auto wide = store.recall(1, "funes", "cherries bruise fast", 8);
    std::set<int64_t> seen;
    for (const auto& m : wide) CHECK(seen.insert(m.id).second);
    return 0;
}

int test_keyword_only_mode_still_works() {
    // No embedder: recall falls back to keyword search, and the link
    // expansion has to work there too — a database with no embedding endpoint
    // is the configuration a fresh install starts in.
    MemoryStore store(temp_db("keyword"), nullptr);

    const int64_t anchor  = store.remember(1, "funes", "kayak kayak kayak", "user");
    const int64_t distant = store.remember(1, "funes", "brief worlds shone", "user");
    CHECK(store.link_memories(1, anchor, distant, "related", 1.0));

    auto hits = store.recall(1, "funes", "kayak", 5);
    CHECK(has_id(hits, anchor));
    CHECK(has_id(hits, distant));
    return 0;
}

int test_the_backfill_asks_once_and_only_about_plausible_pairs() {
    FakeEmbedder embedder;
    MemoryStore store(temp_db("backfill"), &embedder);

    store.remember(1, "funes", "the roof leaks when it rains", "user");
    store.remember(1, "funes", "the roof leaks after rain", "user");     // near-duplicate
    store.remember(1, "funes", "roofers quoted eight hundred", "user");  // related
    store.remember(1, "funes", "zqx wjvk pmbg", "user");                 // unrelated

    std::vector<std::pair<std::string, std::string>> asked;
    auto judge = [&](const std::string& a, const std::string& b) {
        asked.push_back({a, b});
        return "related";
    };

    // The band is set from the fixture's own measured similarities rather than
    // from the defaults: what is being asserted is that the band excludes the
    // near-duplicate and the stranger, not that 0.55/0.92 happen to fall
    // between them for this toy embedder.
    const double dup_sim     = cosine(embedder, "the roof leaks when it rains",
                                                "the roof leaks after rain");
    const double related_sim = cosine(embedder, "the roof leaks when it rains",
                                                "roofers quoted eight hundred");
    const double far_sim     = cosine(embedder, "the roof leaks when it rains",
                                                "zqx wjvk pmbg");
    CHECK(far_sim < related_sim && related_sim < dup_sim);   // the fixture is sane

    MemoryStore::LinkBackfillOptions opt;
    opt.user_id = 1;
    opt.min_similarity = (far_sim + related_sim) / 2;
    opt.max_similarity = (related_sim + dup_sim) / 2;
    auto r1 = store.backfill_links(judge, opt);
    CHECK(r1.judged > 0);
    CHECK(r1.linked == r1.judged);

    // Nothing outside the similarity band was ever put to the model: a pair
    // with nothing in common is the model being asked to invent a connection,
    // and a near-identical pair is consolidate()'s work, not a link.
    for (const auto& [a, b] : asked) {
        CHECK(a.find("zqx") == std::string::npos);
        CHECK(b.find("zqx") == std::string::npos);
        CHECK(!(a == "the roof leaks when it rains" && b == "the roof leaks after rain"));
    }

    // A second run does not re-ask what the first one answered. This is what
    // makes a capped nightly pass finish the pool instead of redoing the
    // cheapest questions forever.
    const size_t asked_first = asked.size();
    auto r2 = store.backfill_links(judge, opt);
    CHECK(asked.size() == asked_first);
    CHECK(r2.judged == 0);

    // A "no" is remembered as firmly as a "yes".
    MemoryStore store2(temp_db("backfill_no"), &embedder);
    store2.remember(1, "funes", "the roof leaks when it rains", "user");
    store2.remember(1, "funes", "roofers quoted eight hundred", "user");
    int calls = 0;
    auto refuse = [&](const std::string&, const std::string&) { ++calls; return ""; };
    MemoryStore::LinkBackfillOptions opt2;
    opt2.user_id = 1;
    opt2.min_similarity = opt.min_similarity;
    opt2.max_similarity = opt.max_similarity;
    auto n1 = store2.backfill_links(refuse, opt2);
    CHECK(n1.judged > 0 && n1.linked == 0);
    CHECK(store2.count_links(1) == 0);
    const int after_first = calls;
    store2.backfill_links(refuse, opt2);
    CHECK(calls == after_first);

    // A judge that throws leaves the pair unjudged — a model being down is
    // not evidence that two memories are unrelated.
    MemoryStore store3(temp_db("backfill_throw"), &embedder);
    store3.remember(1, "funes", "the roof leaks when it rains", "user");
    store3.remember(1, "funes", "roofers quoted eight hundred", "user");
    auto thrower = [](const std::string&, const std::string&) -> std::string {
        throw std::runtime_error("endpoint down");
    };
    store3.backfill_links(thrower, opt2);
    int retried = 0;
    auto counting = [&](const std::string&, const std::string&) { ++retried; return ""; };
    store3.backfill_links(counting, opt2);
    CHECK(retried > 0);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_links_are_stored_and_scoped_to_one_account();
    rc |= test_deleting_a_memory_takes_its_links();
    rc |= test_recall_reaches_a_linked_memory_it_could_not_match();
    rc |= test_an_expanded_hit_cannot_outrank_its_anchor();
    rc |= test_term_matches_fill_slots_without_displacing();
    rc |= test_recall_is_unchanged_when_there_is_nothing_to_add();
    rc |= test_keyword_only_mode_still_works();
    rc |= test_the_backfill_asks_once_and_only_about_plausible_pairs();
    if (rc == 0) std::cout << "test_memory_links: all passed\n";
    return rc;
}
