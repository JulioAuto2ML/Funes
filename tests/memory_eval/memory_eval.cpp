// =============================================================================
// tests/memory_eval/memory_eval.cpp — how good is recall, in numbers
// =============================================================================
// docs/memory-plan.md, phase 1. Before this nothing measured recall: the 1.3
// source weight was tuned by hand against one query, and no change to the
// memory model could be shown to help or hurt. This loads a fixture pool into
// a throwaway database through the same write path the agent uses
// (MemoryStore::remember, funes::auto_memory_text for exchanges), asks each
// question through MemoryStore::recall, and scores the top k.
//
// Not a pass/fail test. It is a report, run before and after a change to the
// memory model, with the numbers going into the commit or PR that made it.
// The ctest entry only checks that it runs.
//
//   memory_eval [--fixture PATH] [--k N] [--embed-url URL] [--embed-model M]
//               [--extract-llm URL] [--extract-model M] [--verbose] [--json]
//
// With --extract-llm, exchanges are not stored as `User said …` logs: they
// are queued and put through the fact extraction pass (fact_extraction.h)
// against that model, as FUNES_AUTO_MEMORY=extract does. A fact counts as its
// exchange for scoring (a fact from x_bill answers what x_bill answered); an
// exchange that yields nothing simply is not in the pool any more.
//
// Without an embedding endpoint (--embed-url, else FUNES_EMBED_URL) recall is
// keyword-only, which is what a deployment without one gets — and is a
// different, much worse, number. The report says which mode it measured.
//
// Metrics, all averaged over the queries:
//   recall@k         share of a query's relevant memories that are in the top k
//   hit@1            the first memory is a relevant one
//   stale-first      a stale or contradicting memory outranks every relevant
//                    one (or is present when no relevant one is) — the "I
//                    don't have your name" failure
//   contamination@k  share of the k slots held by a memory that is stale for
//                    this query, or marked noise and not relevant to it

#include "auto_memory.h"
#include "fact_extraction.h"
#include "json.hpp"
#include "llm_client.h"
#include "memory.h"
#include "memory_provenance.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Args {
    std::string fixture = FUNES_EVAL_FIXTURE;
    int         k = 4;
    std::string embed_url;
    std::string embed_model = "default";
    bool        verbose = false;
    bool        as_json = false;
    std::string extract_url;
    std::string extract_model = "default";
};

bool parse_args(int argc, char** argv, Args& a) {
    if (const char* u = std::getenv("FUNES_EMBED_URL")) a.embed_url = u;
    if (const char* m = std::getenv("FUNES_EMBED_MODEL")) a.embed_model = m;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if      (arg == "--fixture")     a.fixture = next();
        else if (arg == "--k")           a.k = std::atoi(next().c_str());
        else if (arg == "--embed-url")   a.embed_url = next();
        else if (arg == "--embed-model") a.embed_model = next();
        else if (arg == "--no-embed")    a.embed_url.clear();
        else if (arg == "--extract-llm")   a.extract_url = next();
        else if (arg == "--extract-model") a.extract_model = next();
        else if (arg == "--verbose")     a.verbose = true;
        else if (arg == "--json")        a.as_json = true;
        else { std::cerr << "memory_eval: unknown argument " << arg << "\n"; return false; }
    }
    return a.k > 0;
}

std::string label(const std::map<int64_t, std::string>& keys, int64_t id) {
    auto it = keys.find(id);
    return it == keys.end() ? "#" + std::to_string(id) : it->second;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 2;

    json fixture;
    try {
        std::ifstream in(args.fixture);
        fixture = json::parse(in);
    } catch (const std::exception& e) {
        std::cerr << "memory_eval: cannot read " << args.fixture << ": " << e.what() << "\n";
        return 2;
    }

    const fs::path db = fs::temp_directory_path()
                      / ("funes_memory_eval_" + std::to_string(getpid()) + ".db");
    fs::remove(db);

    std::unique_ptr<EmbeddingClient> embedder;
    if (!args.embed_url.empty())
        embedder = std::make_unique<EmbeddingClient>(args.embed_url, "", args.embed_model);

    constexpr int64_t kUser = 1;
    const std::string kAgent = "funes";

    std::map<std::string, int64_t> ids;     // fixture key → memory id
    std::map<int64_t, std::string> keys;    // and back, for the report
    std::set<std::string> noise;
    json report = {{"queries", json::array()}};
    double sum_recall = 0, sum_hit1 = 0, sum_stale_first = 0, sum_contam = 0;
    size_t n = 0;
    bool semantic = false;
    size_t pool = 0;                        // memories in the pool when queried

    {
        MemoryStore memory(db.string(), embedder.get());

        for (const auto& m : fixture["memories"]) {
            const std::string key = m["key"];
            std::string text, source;
            if (m.contains("user") && !args.extract_url.empty()) {
                // Queued the way agent.cpp queues it; scored after the pass.
                memory.enqueue_exchange(kUser, kAgent, "eval:" + key,
                    funes::shorten_on_sentence(funes::without_attachments(m["user"]), 2000),
                    funes::shorten_on_sentence(m["reply"].get<std::string>(), 2000),
                    {"session:eval:" + key, false});
                if (m.value("noise", false)) noise.insert(key);
                continue;
            }
            if (m.contains("user")) {
                text   = funes::auto_memory_text(m["user"], m["reply"]);
                source = "auto";
            } else {
                text   = m["text"];
                source = m.value("source", "tool");
            }
            // A memory written after the run read third-party content, stored
            // the way the remember tool stores it.
            funes::Provenance prov;
            prov.origin = m.value("tainted_by", "fixture");
            prov.tainted = m.contains("tainted_by");
            const int64_t id = memory.remember(kUser, kAgent, text, source, prov);
            ids[key] = id;
            keys[id] = key;
            if (m.value("noise", false)) noise.insert(key);
        }
        if (!args.extract_url.empty()) {
            LLMClient llm(args.extract_url, "", args.extract_model, "openai");
            llm.set_max_tokens(512);
            // As many passes as production gets (a failed exchange is
            // retried on later polls until kMaxExtractAttempts).
            funes::FactExtractionReport r;
            for (int pass = 0; pass < MemoryStore::kMaxExtractAttempts
                               && memory.count_pending_exchanges() > 0; ++pass) {
                const auto p = funes::run_fact_extraction(memory,
                    [&](const std::vector<ChatMessage>& msgs) { return llm.complete(msgs).content; },
                    funes::FactExtractionOptions{1000, 6, 0});
                r.seen += p.done + (pass == 0 ? p.failed : 0);
                r.facts += p.facts;
                r.failed = p.failed;
                r.restated += p.restated;
            }
            // Each fact answers for the exchange it came from.
            for (const auto& mem : memory.list(kUser, kAgent, 10000)) {
                if (mem.source != "extracted") continue;
                const std::string prefix = "session:eval:";
                if (mem.origin.rfind(prefix, 0) == 0) keys[mem.id] = mem.origin.substr(prefix.size());
            }
            int retired = 0;
            for (const auto& mem : memory.list(kUser, kAgent, 10000))
                if (mem.superseded_by) {
                    ++retired;
                    if (args.verbose && !args.as_json)
                        std::cout << "  retired " << label(keys, mem.id) << " by "
                                  << label(keys, mem.superseded_by) << "\n";
                }
            report["extraction"] = {{"exchanges", r.seen}, {"facts", r.facts},
                                    {"failed", r.failed}, {"restated", r.restated},
                                    {"superseded", retired}};
            if (args.verbose && !args.as_json)
                for (const auto& mem : memory.list(kUser, kAgent, 10000))
                    if (mem.source == "extracted")
                        std::cout << "  fact " << label(keys, mem.id) << ": " << mem.text << "\n";
        }
        semantic = memory.semantic_available();
        pool = static_cast<size_t>(memory.count(kUser, kAgent));

        for (const auto& q : fixture["queries"]) {
            const std::string question = q["q"];
            std::set<std::string> relevant, stale;
            for (const auto& r : q["relevant"]) relevant.insert(r.get<std::string>());
            for (const auto& s : q["stale"])    stale.insert(s.get<std::string>());

            const auto hits = memory.recall(kUser, kAgent, question, args.k, /*touch=*/false);

            std::vector<std::string> got;
            for (const auto& h : hits) got.push_back(label(keys, h.id));

            size_t found = 0, contaminated = 0;
            int first_relevant = -1, first_stale = -1;
            for (size_t i = 0; i < got.size(); ++i) {
                const bool is_rel = relevant.count(got[i]) > 0;
                const bool is_stale = stale.count(got[i]) > 0;
                if (is_rel) { ++found; if (first_relevant < 0) first_relevant = static_cast<int>(i); }
                if (is_stale && first_stale < 0) first_stale = static_cast<int>(i);
                if (is_stale || (noise.count(got[i]) && !is_rel)) ++contaminated;
            }
            const double rec = relevant.empty() ? 1.0
                             : static_cast<double>(found) / static_cast<double>(relevant.size());
            const bool hit1 = !got.empty() && relevant.count(got[0]);
            const bool stale_first = first_stale >= 0
                                  && (first_relevant < 0 || first_stale < first_relevant);
            const double contam = static_cast<double>(contaminated) / args.k;

            sum_recall += rec;
            sum_hit1 += hit1 ? 1 : 0;
            sum_stale_first += stale_first ? 1 : 0;
            sum_contam += contam;
            ++n;

            report["queries"].push_back({{"q", question}, {"top", got}, {"recall", rec},
                                         {"hit1", hit1}, {"stale_first", stale_first},
                                         {"contamination", contam}});
            if (args.verbose && !args.as_json) {
                std::cout << (hit1 ? "  ok  " : "  --  ") << (stale_first ? "STALE " : "      ")
                          << question << "\n        →";
                for (size_t i = 0; i < hits.size(); ++i)
                    std::cout << " " << got[i] << std::fixed << std::setprecision(3)
                              << "(" << hits[i].score << ")";
                std::cout << "\n";
            }
        }
    }
    fs::remove(db);

    const double dn = n ? static_cast<double>(n) : 1.0;
    report["mode"]            = semantic ? "semantic" : "keyword";
    report["k"]               = args.k;
    report["queries_n"]       = n;
    report["memories_n"]      = pool;
    report["recall_at_k"]     = sum_recall / dn;
    report["hit_at_1"]        = sum_hit1 / dn;
    report["stale_first"]     = sum_stale_first / dn;
    report["contamination"]   = sum_contam / dn;

    if (args.as_json) {
        std::cout << report.dump(2) << "\n";
    } else {
        std::cout << std::fixed << std::setprecision(3)
                  << "memory_eval: " << n << " queries over " << pool << " memories, "
                  << report["mode"].get<std::string>() << " recall, k=" << args.k
                  << (args.extract_url.empty() ? ", exchanges logged" : ", exchanges extracted")
                  << "\n"
                  << "  recall@" << args.k << "        " << sum_recall / dn << "\n"
                  << "  hit@1           " << sum_hit1 / dn << "\n"
                  << "  stale-first     " << sum_stale_first / dn << "   (lower is better)\n"
                  << "  contamination@" << args.k << " " << sum_contam / dn << "   (lower is better)\n";
    }
    return 0;
}
