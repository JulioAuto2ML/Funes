// =============================================================================
// src/core/fact_extraction.cpp — facts instead of exchanges
// =============================================================================

#include "fact_extraction.h"
#include "answer_schema.h"
#include "auto_memory.h"
#include <iostream>
#include <set>
#include <sstream>

namespace funes {

using nlohmann::json;

const json& fact_schema() {
    static const json schema = {
        {"type", "object"},
        {"required", {"facts"}},
        {"properties", {
            {"facts", {
                {"type", "array"},
                {"items", {
                    {"type", "object"},
                    {"required", {"text", "kind"}},
                    {"properties", {
                        {"text", {{"type", "string"}}},
                        {"kind", {{"enum", {"about_person", "preference", "plan",
                                            "decision", "event"}}}}
                    }}
                }}
            }}
        }}
    };
    return schema;
}

namespace {

// Blunt on purpose, like merge_memories and judge_link in main.cpp: this runs
// unattended against a local model, and every hedge it is allowed becomes a
// stored memory. The rules are the defects in docs/memory-plan.md, in order:
// no exchanges, no meta, no negatives, no relative dates.
const char* const kSystem =
    "You maintain a long-term memory of facts about one person, built from their "
    "conversations with their assistant. You are shown facts already remembered "
    "and one new exchange. Reply with JSON only, no other text:\n"
    "{\"facts\": [{\"text\": \"...\", \"kind\": \"...\", \"replaces\": null}]}\n\n"
    "Keep a fact only if it will still be true and useful weeks from now: who the "
    "person is, the people and things in their life, their preferences, plans and "
    "decisions, and things that were actually done (a job scheduled, a bill paid, "
    "a purchase made).\n\n"
    "Rules:\n"
    "- An empty list is the usual, correct answer. Greetings, thanks, questions, "
    "small talk, and anything about the assistant itself or its memory produce no "
    "facts.\n"
    "- Only what the person said or confirmed, or what the assistant reports having "
    "actually done. Never the assistant's opinions, suggestions, guesses or answers "
    "to questions — an assistant's answer may be wrong — and never that the "
    "assistant does not know something.\n"
    "- When the assistant lists what it remembers or summarises past "
    "conversations, that is not new: produce no facts from it.\n"
    "- Each fact is one short self-contained sentence. Name the person (use the "
    "name if a remembered fact gives it, else \"the user\") instead of a pronoun, "
    "and write dates as dates, not \"tomorrow\" or \"next week\".\n"
    "- Write each fact in the language the person used.\n"
    "- kind is one of: about_person, preference, plan, decision, event.\n"
    "- If a new fact makes a remembered fact outdated or wrong, set replaces to "
    "that remembered fact's number. Otherwise null.\n"
    "- Do not restate a fact that is already remembered.";

int64_t as_id(const json& v) {
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_string()) {
        try { return std::stoll(v.get<std::string>()); } catch (...) {}
    }
    return 0;
}

} // namespace

std::vector<ChatMessage> extraction_messages(const std::string& user_text,
                                             const std::string& reply,
                                             const std::vector<MemoryStore::Memory>& known,
                                             const std::string& date) {
    std::ostringstream u;
    u << "Already remembered:\n";
    if (known.empty()) u << "(nothing relevant)\n";
    for (const auto& m : known) u << "[" << m.id << "] " << m.presented() << "\n";
    u << "\nExchange (" << date << "):\n"
      << "Person: " << user_text << "\n"
      << "Assistant: " << reply << "\n";

    std::vector<ChatMessage> msgs(2);
    msgs[0].role = "system";
    msgs[0].content = kSystem;
    msgs[1].role = "user";
    msgs[1].content = u.str();
    return msgs;
}

bool parse_facts(const std::string& answer, const std::vector<MemoryStore::Memory>& known,
                 std::vector<ExtractedFact>& out, std::string& error) {
    out.clear();
    json j;
    if (!extract_answer_json(answer, j)) { error = "no JSON in the answer"; return false; }
    // A bare array is the one shape small models produce often enough to be
    // worth accepting: it is the facts list with the wrapper forgotten.
    if (j.is_array()) j = json{{"facts", j}};
    if (std::string e = validate_answer(fact_schema(), j); !e.empty()) {
        error = e;
        return false;
    }

    // What an extracted fact may retire: another extracted fact or an old
    // exchange log — what extraction itself produces or replaces. A taught
    // fact, a remember or a consolidated memory is retired only by the
    // pairwise contradiction judge or by the person: measured on
    // tests/memory_eval, an 8B model's `replaces` retired a richer memory
    // with a narrower paraphrase, and a true fact with the assistant's
    // mistaken claim.
    std::set<int64_t> replaceable;
    for (const auto& m : known)
        if (m.source == "extracted" || m.source == "auto") replaceable.insert(m.id);

    for (const auto& f : j["facts"]) {
        ExtractedFact fact;
        fact.text = f["text"].get<std::string>();
        while (!fact.text.empty() && std::isspace(static_cast<unsigned char>(fact.text.back())))
            fact.text.pop_back();
        if (fact.text.empty()) continue;
        fact.text = shorten_on_sentence(fact.text, kMaxFactBytes);
        fact.kind = f["kind"].get<std::string>();
        if (f.contains("replaces")) {
            const int64_t id = as_id(f["replaces"]);
            fact.claims_replacement = id > 0;
            if (replaceable.count(id)) fact.replaces = id;
        }
        out.push_back(std::move(fact));
    }
    return true;
}

FactExtractionReport run_fact_extraction(MemoryStore& memory, const CompleteFn& complete,
                                         const FactExtractionOptions& opt) {
    FactExtractionReport report;
    for (const auto& e : memory.pending_exchanges(opt.max_items, opt.min_age_seconds)) {
        ++report.seen;
        const auto known = memory.recall(e.user_id, e.agent, e.user_text, opt.known_k,
                                         /*touch=*/false);
        const std::string date = e.created_at.substr(0, 10);

        std::string answer;
        try {
            answer = complete(extraction_messages(e.user_text, e.reply, known, date));
        } catch (const std::exception& ex) {
            std::cerr << "[facts] exchange " << e.id << ": model call failed: " << ex.what() << "\n";
            memory.finish_exchange(e.id, false, 0);
            ++report.failed;
            continue;
        }

        std::vector<ExtractedFact> facts;
        std::string error;
        if (!parse_facts(answer, known, facts, error)) {
            // The answer's head goes in the log: "no JSON" alone does not say
            // whether the model wrote prose, thought aloud or was cut off.
            std::string head = answer.substr(0, 240);
            for (char& c : head) if (c == '\n') c = ' ';
            std::cerr << "[facts] exchange " << e.id << ": unusable answer (" << error
                      << "): " << head << "\n";
            memory.finish_exchange(e.id, false, 0);
            ++report.failed;
            continue;
        }

        Provenance prov;
        prov.tainted = e.tainted;
        prov.origin  = e.tainted ? e.origin : "session:" + e.session;
        int stored = 0;
        for (const auto& f : facts) {
            try {
                // An update the model could not be trusted to apply (its target
                // was taught, not extracted) is still stored; the link judge's
                // contradicts verdict decides between the two.
                if (!f.claims_replacement) {
                    const auto [near_id, sim] = memory.nearest_live(e.user_id, e.agent, f.text);
                    if (near_id && sim >= opt.restatement_similarity) {
                        ++report.restated;
                        std::cerr << "[facts] exchange " << e.id << ": restates #" << near_id
                                  << " (" << sim << "), skipped: " << f.text << "\n";
                        continue;
                    }
                }
                const int64_t id = memory.remember(e.user_id, e.agent, f.text, "extracted", prov);
                ++stored;
                std::cerr << "[facts] exchange " << e.id << " → #" << id << " (" << f.kind
                          << "): " << f.text << "\n";
                if (f.replaces > 0 && f.replaces != id
                    && memory.supersede(e.user_id, f.replaces, id))
                    ++report.replaced;
            } catch (const std::exception& ex) {
                std::cerr << "[facts] exchange " << e.id << ": store failed: " << ex.what() << "\n";
            }
        }
        memory.finish_exchange(e.id, true, stored);
        ++report.done;
        report.facts += stored;
    }
    return report;
}

} // namespace funes
