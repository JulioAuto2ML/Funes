// =============================================================================
// tests/test_citation.cpp — eviction by use, not by recall (memory plan, phase 7)
// =============================================================================
// Recall puts four memories in front of the model on every turn, so "was
// recalled" protected clutter that matched the questions it came from (175
// of yoda's 209 exchange logs). A memory now counts as used when a reply
// repeats its distinctive words, and the never-used are archived — after a
// grace window, never deleted.

#include "agent.h"
#include "citation.h"
#include "httplib.h"
#include "memory.h"
#include "permissions.h"
#include "sqlite3.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace fs = std::filesystem;

namespace {

std::string fresh_db(const std::string& name) {
    const fs::path p = fs::temp_directory_path() / ("funes_test_citation_" + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

// Backdating through a second connection: MemoryStore has no API to forge
// timestamps, and should not grow one for a test.
void sql(const std::string& db, const std::string& stmt) {
    sqlite3* h = nullptr;
    sqlite3_open(db.c_str(), &h);
    sqlite3_exec(h, stmt.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(h);
}

bool archived(MemoryStore& m, int64_t id) {
    for (const auto& mem : m.list(1, "funes", 1000))
        if (mem.id == id) return mem.archived;
    return false;
}

} // namespace

int test_is_cited() {
    using funes::is_cited;
    CHECK(is_cited("The Spanish newsletter goes out on Tuesday, Thursday and Saturday.",
                   "It goes out Tuesday and Saturday, plus Thursday."));
    CHECK(!is_cited("The Spanish newsletter goes out on Tuesday, Thursday and Saturday.",
                    "Hola, ¿en qué te ayudo?"));
    // One distinctive word is not a citation of a memory that has several.
    CHECK(!is_cited("Julio is considering a Toyota Corolla Cross hybrid.", "Hola Julio!"));
    // Short memories: all of their (one or two) terms.
    CHECK(is_cited("El usuario se llama Julio.", "Te llamás Julio, y llama la atención."));
    CHECK(is_cited("Yerba.", "Your yerba is ready."));
    CHECK(!is_cited("Prefers yerba.", "Your yerba is ready."));   // one of two
    // Shared topic words alone: not a citation of another memory on the topic.
    CHECK(!is_cited("The Spanish newsletter has a Sunday carousel on LinkedIn.",
                    "The Spanish newsletter goes out Tuesday, Thursday and Saturday."));
    // Common words and "user" carry nothing.
    CHECK(funes::content_terms("The user said that this would have been there").empty());
    // Case-insensitive; accented words kept whole.
    CHECK(is_cited("Edenor factura vencimiento", "La FACTURA de EDENOR vence pronto"));
    CHECK(funes::content_terms("también vencimiento").count("vencimiento") == 1);
    CHECK(funes::content_terms("también vencimiento").count("también") == 0);
    return 0;
}

int test_archive_uncited_respects_grace_links_and_source() {
    const std::string db = fresh_db("archive");
    MemoryStore m(db, nullptr);
    const int64_t unused   = m.remember(1, "funes", "An extracted fact nobody uses.", "extracted");
    const int64_t used     = m.remember(1, "funes", "An extracted fact a reply used.", "extracted");
    const int64_t log      = m.remember(1, "funes", "User said: \"hi\" — I replied: \"hi\"", "auto");
    const int64_t linked   = m.remember(1, "funes", "An extracted fact with a link.", "extracted");
    const int64_t anchor   = m.remember(1, "funes", "Something it is linked to.", "tool");
    const int64_t taught   = m.remember(1, "funes", "A fact taught by hand.", "user");
    CHECK(m.link_memories(1, linked, anchor));
    m.mark_cited(1, {used});
    m.mark_cited(2, {unused});              // another account cannot cite my memory

    sql(db, "UPDATE memories SET created_at = datetime('now', '-200 days')");

    MemoryStore::ConsolidationOptions opt;
    opt.user_id = 1;
    opt.prune_after_days = -1;   // step 2 would delete the never-recalled log first

    // Counting only began now: nothing is judged yet, however old.
    auto r = m.consolidate([](const std::vector<std::string>&) { return std::string("KEEP ALL"); }, opt);
    CHECK(r.archived == 0);

    // A full window after counting began.
    sql(db, "UPDATE meta SET value = datetime('now', '-100 days') WHERE key = 'cited_since'");
    r = m.consolidate([](const std::vector<std::string>&) { return std::string("KEEP ALL"); }, opt);
    CHECK(r.archived == 2);
    CHECK(archived(m, unused));
    CHECK(archived(m, log));
    CHECK(!archived(m, used));
    CHECK(!archived(m, linked));
    CHECK(!archived(m, anchor));
    CHECK(!archived(m, taught));
    CHECK(m.count(1, "funes") == 6);        // archived, not deleted

    // A young memory is not judged: it has had no window to be cited in.
    const int64_t young = m.remember(1, "funes", "A brand new extracted fact.", "extracted");
    r = m.consolidate([](const std::vector<std::string>&) { return std::string("KEEP ALL"); }, opt);
    CHECK(r.archived == 0);
    CHECK(!archived(m, young));
    return 0;
}

// Through the agent: a reply that repeats a recalled memory's words counts as
// a citation of it; one that does not, doesn't.
int test_agent_counts_citations() {
    httplib::Server llm;
    llm.Post("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"choices":[{"message":{"role":"assistant",)"
                        R"("content":"The Spanish newsletter goes out Tuesday, Thursday and Saturday."}}]})",
                        "application/json");
    });
    const int port = llm.bind_to_any_port("127.0.0.1");
    std::thread th([&] { llm.listen_after_bind(); });
    for (int i = 0; i < 50 && !llm.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    int rc = 0;
    {
        MemoryStore memory(fresh_db("agent"), nullptr);
        const int64_t days = memory.remember(1, "funes",
            "The Spanish newsletter goes out on Tuesday, Thursday and Saturday.", "tool");
        const int64_t other = memory.remember(1, "funes",
            "The Spanish newsletter has a Sunday carousel on LinkedIn.", "tool");
        ToolRegistry tools;
        AgentDefaults defaults;
        defaults.llm_url = "http://127.0.0.1:" + std::to_string(port);
        AgentConfig cfg;
        cfg.name = "funes";
        cfg.memory_scope = "funes";
        cfg.system_prompt = "test";
        FunesAgent agent(cfg, tools, memory, defaults);
        agent.run("Spanish newsletter", "s1", 1, funes::Permissions::unrestricted(), nullptr, {},
                  Persist::TurnsOnly);
        for (const auto& m : memory.list(1, "funes")) {
            if (m.id == days && m.cited_count != 1) rc = 1;
            if (m.id == other && m.cited_count != 0) rc = 1;
        }
    }
    llm.stop();
    th.join();
    CHECK(rc == 0);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_is_cited();
    rc |= test_archive_uncited_respects_grace_links_and_source();
    rc |= test_agent_counts_citations();
    if (rc == 0) std::cout << "test_citation: all tests passed\n";
    return rc;
}
