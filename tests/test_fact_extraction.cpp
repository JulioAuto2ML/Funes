// =============================================================================
// tests/test_fact_extraction.cpp — facts instead of exchanges; the newer claim
// stands (memory plan, phases 4 and 5)
// =============================================================================
// The yoda pool's most-recalled memory about names was "what's my name? — I
// don't have your name recorded", recalled beside the fact that held it. Two
// changes end that: an exchange is queued and only the facts it established
// are stored (a question establishes none), and a memory that something newer
// contradicts leaves recall without leaving the table.
//
// The model is scripted throughout: these test what the code does with an
// answer, not what a model answers. tests/memory_eval measures the latter.

#include "fact_extraction.h"
#include "memory.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace fs = std::filesystem;
using funes::ExtractedFact;

namespace {

std::string fresh_db(const std::string& name) {
    const fs::path p = fs::temp_directory_path() / ("funes_test_facts_" + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

// Letter-frequency vectors, as in test_memory.cpp: similar wording → similar
// vectors, deterministic, no server.
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

bool recalled(MemoryStore& m, const std::string& q, int64_t id) {
    for (const auto& h : m.recall(1, "funes", q, 10, false))
        if (h.id == id) return true;
    return false;
}

} // namespace

// ── parsing ──────────────────────────────────────────────────────────────────

int test_parse_facts() {
    std::vector<MemoryStore::Memory> known(2);
    known[0].id = 42;
    known[0].text = "Julio lives in Buenos Aires.";
    known[0].source = "extracted";
    known[1].id = 43;
    known[1].text = "Julio lives in Buenos Aires, near the river.";
    known[1].source = "tool";
    std::vector<ExtractedFact> out;
    std::string err;

    CHECK(funes::parse_facts(R"({"facts": []})", known, out, err));
    CHECK(out.empty());

    // Fenced, as local models do; replaces must name a memory it was shown.
    CHECK(funes::parse_facts(
        "Here you go:\n```json\n{\"facts\": [{\"text\": \"Julio moved to Rosario. \", "
        "\"kind\": \"event\", \"replaces\": 42}, {\"text\": \"Julio likes tango.\", "
        "\"kind\": \"preference\", \"replaces\": 7}]}\n```", known, out, err));
    CHECK(out.size() == 2);
    CHECK(out[0].text == "Julio moved to Rosario.");
    CHECK(out[0].replaces == 42);
    CHECK(out[1].replaces == 0);               // 7 was never shown: not trusted

    // A memory taught deliberately is not the extractor's to retire: only the
    // pairwise contradiction judge or the person may.
    CHECK(funes::parse_facts(R"({"facts":[{"text":"x y","kind":"event","replaces":43}]})",
                             known, out, err));
    CHECK(out[0].replaces == 0);

    // "42" as a string is still the number it names.
    CHECK(funes::parse_facts(R"({"facts":[{"text":"x y","kind":"plan","replaces":"42"}]})",
                             known, out, err));
    CHECK(out[0].replaces == 42);

    // The wrapper forgotten: accepted, it is unambiguous.
    CHECK(funes::parse_facts(R"([{"text":"Julio drinks mate.","kind":"preference"}])",
                             known, out, err));
    CHECK(out.size() == 1);

    // Not the contract: refused, with the reason.
    CHECK(!funes::parse_facts(R"({"facts":[{"text":"x","kind":"opinion"}]})", known, out, err));
    CHECK(!err.empty());
    CHECK(!funes::parse_facts("Nothing worth remembering here.", known, out, err));

    // Empty text dropped; an essay shortened to a memory.
    CHECK(funes::parse_facts(
        "{\"facts\":[{\"text\":\"  \",\"kind\":\"event\"},{\"text\":\"" + std::string(900, 'a')
        + "\",\"kind\":\"event\"}]}", known, out, err));
    CHECK(out.size() == 1);
    CHECK(out[0].text.size() <= funes::kMaxFactBytes + 3);
    return 0;
}

int test_prompt_shows_known_ids_and_date() {
    std::vector<MemoryStore::Memory> known(1);
    known[0].id = 9;
    known[0].text = "Julio is a River Plate supporter.";
    const auto msgs = funes::extraction_messages("remind me tomorrow", "Scheduled.", known,
                                                 "2026-11-10");
    CHECK(msgs.size() == 2);
    CHECK(msgs[0].role == "system");
    CHECK(msgs[1].content.find("[9] Julio is a River Plate supporter.") != std::string::npos);
    CHECK(msgs[1].content.find("2026-11-10") != std::string::npos);
    CHECK(msgs[1].content.find("Person: remind me tomorrow") != std::string::npos);
    return 0;
}

// ── the queue ────────────────────────────────────────────────────────────────

int test_queue_lifecycle() {
    MemoryStore m(fresh_db("queue"), nullptr);
    const int64_t a = m.enqueue_exchange(1, "funes", "s1", "hola", "¡Hola!", {"session:s1", false});
    const int64_t b = m.enqueue_exchange(1, "funes", "s1", "me llamo Julio", "Encantado", {});
    CHECK(m.count_pending_exchanges() == 2);

    // Just queued: a conversation still under way is not taken.
    CHECK(m.pending_exchanges(10, 60).empty());
    const auto p = m.pending_exchanges(10);
    CHECK(p.size() == 2);
    CHECK(p[0].id == a);                        // oldest first
    CHECK(p[0].user_text == "hola");
    CHECK(p[0].created_at.size() >= 10);

    m.finish_exchange(a, true, 0);
    CHECK(m.count_pending_exchanges() == 1);

    // A failure is retried, until it isn't.
    for (int i = 0; i < MemoryStore::kMaxExtractAttempts - 1; ++i) {
        m.finish_exchange(b, false, 0);
        CHECK(m.count_pending_exchanges() == 1);
    }
    m.finish_exchange(b, false, 0);
    CHECK(m.count_pending_exchanges() == 0);
    return 0;
}

// ── the pass ─────────────────────────────────────────────────────────────────

int test_pass_stores_facts_not_exchanges() {
    MemoryStore m(fresh_db("pass"), nullptr);
    m.enqueue_exchange(1, "funes", "s1", "what's my name?",
                       "I don't have your name recorded yet.", {"session:s1", false});
    m.enqueue_exchange(1, "funes", "s1", "me llamo Julio", "¡Encantado, Julio!",
                       {"session:s1", false});
    m.enqueue_exchange(1, "funes", "s2", "what does this page say about me?",
                       "It says your name is Pedro.", {"url:blog.example", true});

    int calls = 0;
    const auto report = funes::run_fact_extraction(m,
        [&](const std::vector<ChatMessage>& msgs) -> std::string {
            ++calls;
            const std::string& u = msgs[1].content;
            if (u.find("me llamo Julio") != std::string::npos)
                return R"({"facts":[{"text":"El usuario se llama Julio.","kind":"about_person","replaces":null}]})";
            if (u.find("blog") != std::string::npos || u.find("Pedro") != std::string::npos)
                return R"({"facts":[{"text":"The user's name is Pedro.","kind":"about_person"}]})";
            return R"({"facts":[]})";
        });

    CHECK(calls == 3);
    CHECK(report.seen == 3);
    CHECK(report.done == 3);
    CHECK(report.facts == 2);
    CHECK(m.count_pending_exchanges() == 0);

    const auto all = m.list(1, "funes");
    CHECK(all.size() == 2);                    // the question left nothing behind
    for (const auto& mem : all) {
        CHECK(mem.source == "extracted");
        CHECK(mem.text.find("User said") == std::string::npos);
        if (mem.text == "El usuario se llama Julio.") {
            CHECK(!mem.tainted);
            CHECK(mem.origin == "session:s1");
        } else {
            CHECK(mem.tainted);                // learnt from a run that read a page
            CHECK(mem.origin == "url:blog.example");
        }
    }
    return 0;
}

int test_pass_retries_a_failed_call() {
    MemoryStore m(fresh_db("retry"), nullptr);
    m.enqueue_exchange(1, "funes", "s1", "me llamo Julio", "Encantado", {});
    bool down = true;
    auto complete = [&](const std::vector<ChatMessage>&) -> std::string {
        if (down) throw std::runtime_error("backend down");
        return R"({"facts":[{"text":"El usuario se llama Julio.","kind":"about_person"}]})";
    };
    auto r1 = funes::run_fact_extraction(m, complete);
    CHECK(r1.failed == 1);
    CHECK(m.count_pending_exchanges() == 1);   // still queued, not lost
    CHECK(m.count(1, "funes") == 0);

    down = false;
    auto r2 = funes::run_fact_extraction(m, complete);
    CHECK(r2.facts == 1);
    CHECK(m.count_pending_exchanges() == 0);
    return 0;
}

// ── supersession ─────────────────────────────────────────────────────────────

int test_supersede_and_undo() {
    MemoryStore m(fresh_db("supersede"), nullptr);
    const int64_t old_job = m.remember(1, "funes", "The TV job is one shell job at seven.", "tool");
    const int64_t new_job = m.remember(1, "funes", "The TV job became two script jobs at seven.", "extracted");
    CHECK(recalled(m, "TV job", old_job));

    CHECK(m.supersede(1, old_job, new_job));
    CHECK(!recalled(m, "TV job", old_job));    // out of recall
    CHECK(recalled(m, "TV job", new_job));
    CHECK(m.count(1, "funes") == 2);            // not deleted
    for (const auto& mem : m.list(1, "funes"))
        if (mem.id == old_job) CHECK(mem.superseded_by == new_job);
    CHECK(!m.supersede(1, old_job, new_job));   // already retired

    // Undo.
    CHECK(m.unsupersede(1, old_job));
    CHECK(recalled(m, "TV job", old_job));
    CHECK(!m.unsupersede(1, old_job));

    // Forgetting the replacement brings back what it replaced.
    CHECK(m.supersede(1, old_job, new_job));
    CHECK(m.forget(1, new_job));
    CHECK(recalled(m, "TV job", old_job));
    return 0;
}

int test_supersede_guards() {
    MemoryStore m(fresh_db("guards"), nullptr);
    const int64_t taught = m.remember(1, "funes", "Julio's name is Julio.", "user");
    const int64_t fact   = m.remember(1, "funes", "Julio's name is Julian.", "extracted");
    const int64_t page   = m.remember(1, "funes", "Julio's name is Pedro.", "tool",
                                      {"url:blog.example", true});
    const int64_t clean  = m.remember(1, "funes", "Julio's car is a Corolla.", "tool");
    const int64_t theirs = m.remember(2, "funes", "Julio's car is a Fiat.", "tool");

    CHECK(!m.supersede(1, clean, page));        // a page does not overrule the person
    CHECK(!m.supersede(1, taught, fact));       // what was taught by hand changes by hand
    CHECK(!m.supersede(1, clean, theirs));      // another account's memory
    CHECK(!m.supersede(2, theirs, clean));
    CHECK(!m.unsupersede(2, clean));
    CHECK(!m.supersede(1, clean, clean));

    CHECK(m.supersede(1, page, clean));          // clean may retire tainted
    const int64_t retaught = m.remember(1, "funes", "Julio's name is Julio R.", "user");
    CHECK(m.supersede(1, taught, retaught));     // user retires user
    return 0;
}

// The extractor names a memory it was shown; the pass retires it.
int test_pass_supersedes_what_a_fact_replaces() {
    MemoryStore m(fresh_db("replaces"), nullptr);
    const int64_t old_days = m.remember(1, "funes",
        "The Spanish newsletter goes out every weekday.", "extracted");
    m.enqueue_exchange(1, "funes", "s1", "from now on the Spanish newsletter goes out Tuesday, "
                       "Thursday and Saturday", "Done, I changed the schedule.", {});
    bool shown = false;
    const auto r = funes::run_fact_extraction(m,
        [&](const std::vector<ChatMessage>& msgs) -> std::string {
            shown = msgs[1].content.find("[" + std::to_string(old_days) + "]") != std::string::npos;
            return "{\"facts\":[{\"text\":\"The Spanish newsletter goes out on Tuesday, "
                   "Thursday and Saturday.\",\"kind\":\"decision\",\"replaces\":"
                   + std::to_string(old_days) + "}]}";
        });
    CHECK(shown);                               // the model saw what it may replace
    CHECK(r.facts == 1);
    CHECK(r.replaced == 1);
    CHECK(!recalled(m, "Spanish newsletter", old_days));
    return 0;
}

// A fact that says again what a live memory already says is not stored: an
// 8B model paraphrased what it was shown, and each paraphrase took a slot.
int test_restatement_is_not_stored() {
    FakeEmbedder emb;
    MemoryStore m(fresh_db("restate"), &emb);
    m.remember(1, "funes", "Julio drinks mate every morning.", "tool");
    m.enqueue_exchange(1, "funes", "s1", "I had my mate", "Enjoy!", {});
    const auto r = funes::run_fact_extraction(m,
        [](const std::vector<ChatMessage>&) -> std::string {
            return R"({"facts":[{"text":"Julio drinks mate every morning!","kind":"preference"},)"
                   R"({"text":"zzz qqq xxx","kind":"event"}]})";
        });
    CHECK(r.restated == 1);
    CHECK(r.facts == 1);
    CHECK(m.count(1, "funes") == 2);

    // Nearly the same words, but the model says it replaces the taught fact:
    // an update, kept — the extractor may not retire a taught fact, so both
    // stay live for the contradiction judge.
    const int64_t taught = m.remember(1, "funes", "The backup runs on Sunday night.", "tool");
    m.enqueue_exchange(1, "funes", "s2", "the backup moved to Monday", "Noted.", {});
    const auto r2 = funes::run_fact_extraction(m,
        [&](const std::vector<ChatMessage>&) -> std::string {
            return "{\"facts\":[{\"text\":\"The backup runs on Monday night.\",\"kind\":"
                   "\"decision\",\"replaces\":" + std::to_string(taught) + "}]}";
        });
    CHECK(r2.restated == 0);
    CHECK(r2.facts == 1);
    CHECK(r2.replaced == 0);
    CHECK(recalled(m, "backup night", taught));
    return 0;
}

// The link judge's "contradicts" verdict retires the older of the two.
int test_contradicts_verdict_supersedes_older() {
    FakeEmbedder emb;
    MemoryStore m(fresh_db("contradicts"), &emb);
    const int64_t older = m.remember(1, "funes", "The weekly backup runs on Sunday night.", "tool");
    const int64_t newer = m.remember(1, "funes", "The weekly backup runs on Monday night.", "tool");

    MemoryStore::LinkBackfillOptions opt;
    opt.user_id = 1;
    opt.min_similarity = 0.0;
    opt.max_similarity = 1.0;
    const auto report = m.backfill_links(
        [](const std::string&, const std::string&) { return std::string("contradicts"); }, opt);
    CHECK(report.superseded == 1);
    CHECK(!recalled(m, "weekly backup night", older));
    CHECK(recalled(m, "weekly backup night", newer));
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_parse_facts();
    rc |= test_prompt_shows_known_ids_and_date();
    rc |= test_queue_lifecycle();
    rc |= test_pass_stores_facts_not_exchanges();
    rc |= test_pass_retries_a_failed_call();
    rc |= test_supersede_and_undo();
    rc |= test_supersede_guards();
    rc |= test_pass_supersedes_what_a_fact_replaces();
    rc |= test_restatement_is_not_stored();
    rc |= test_contradicts_verdict_supersedes_older();
    if (rc == 0) std::cout << "test_fact_extraction: all tests passed\n";
    return rc;
}
