// =============================================================================
// tests/memory_eval/memory_probe.cpp — what a migration does to a real pool
// =============================================================================
// memory_eval measures a synthetic pool against labelled answers. This is the
// other half: point it at a *copy* of a real memory.db, give it the questions
// the person actually asks, and it prints the top k for each before and after
// running the phase 6 migration (MemoryStore::enqueue_auto_migration + the
// fact extraction pass) against a real model. No labels, so no score — it is
// for reading. The pool is personal; its output stays out of git.
//
//   memory_probe --db COPY.db --user ID --agent funes --probes FILE
//                --embed-url URL --llm-url URL [--k 4] [--limit N]
//
// Never point --db at a live database: it archives rows and adds facts. Each
// model call waits for busy_gate, so on the production host it yields to chat.

#include "busy_gate.h"
#include "fact_extraction.h"
#include "llm_client.h"
#include "memory.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void show(MemoryStore& memory, int64_t user, const std::string& agent,
          const std::vector<std::string>& probes, int k) {
    for (const auto& q : probes) {
        std::cout << "? " << q << "\n";
        for (const auto& m : memory.recall(user, agent, q, k, /*touch=*/false)) {
            std::string text = m.presented().substr(0, 140);
            for (char& c : text) if (c == '\n') c = ' ';
            std::cout << "    #" << m.id << " " << m.source << " " << m.score << "  " << text << "\n";
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string db, agent = "funes", probes_file, embed_url, llm_url;
    int64_t user = 1;
    int k = 4;
    size_t limit = 100000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if      (a == "--db")        db = next();
        else if (a == "--user")      user = std::atoll(next().c_str());
        else if (a == "--agent")     agent = next();
        else if (a == "--probes")    probes_file = next();
        else if (a == "--embed-url") embed_url = next();
        else if (a == "--llm-url")   llm_url = next();
        else if (a == "--k")         k = std::atoi(next().c_str());
        else if (a == "--limit")     limit = static_cast<size_t>(std::atoll(next().c_str()));
        else { std::cerr << "memory_probe: unknown argument " << a << "\n"; return 2; }
    }
    if (db.empty() || probes_file.empty() || embed_url.empty() || llm_url.empty()) {
        std::cerr << "memory_probe: --db, --probes, --embed-url and --llm-url are required\n";
        return 2;
    }

    std::vector<std::string> probes;
    {
        std::ifstream in(probes_file);
        for (std::string line; std::getline(in, line);)
            if (!line.empty() && line[0] != '#') probes.push_back(line);
    }

    EmbeddingClient embedder(embed_url, "", "default");
    MemoryStore memory(db, &embedder);

    std::cout << "=== before: " << memory.count(user, agent) << " memories\n";
    show(memory, user, agent, probes, k);

    const auto q = memory.enqueue_auto_migration(user, limit, /*dry_run=*/false);
    std::cout << "\n=== migrating " << q.queued << " stored memories (" << q.notes
              << " as notes; " << q.unparsed
              << " auto rows in another shape left alone)\n";

    LLMClient llm(llm_url, "", "default", "openai");
    llm.set_max_tokens(512);
    funes::FactExtractionReport total;
    for (int pass = 0; pass < MemoryStore::kMaxExtractAttempts
                       && memory.count_pending_exchanges() > 0; ++pass) {
        const auto r = funes::run_fact_extraction(memory,
            [&](const std::vector<ChatMessage>& msgs) {
                funes::load::wait_until_quiet();
                return llm.complete(msgs).content;
            }, funes::FactExtractionOptions{100000, 6, 0});
        total.done += r.done;
        total.facts += r.facts;
        total.replaced += r.replaced;
        total.restated += r.restated;
        total.archived += r.archived;
        total.ungrounded += r.ungrounded;
        total.failed = r.failed;
    }
    std::cout << "    " << total.done << " handled, " << total.facts << " fact(s) stored, "
              << total.restated << " restatement(s) dropped, " << total.ungrounded
              << " ungrounded dropped, " << total.replaced
              << " superseded, " << total.archived << " log(s) archived, " << total.failed
              << " still failing\n";

    std::cout << "\n=== extracted facts\n";
    for (const auto& m : memory.list(user, agent, 100000))
        if (m.source == "extracted")
            std::cout << "    #" << m.id << " [" << m.origin << "] " << m.presented() << "\n";

    std::cout << "\n=== after: " << memory.count(user, agent) << " memories, of which "
              << "archived/superseded are out of recall\n";
    show(memory, user, agent, probes, k);
    return 0;
}
