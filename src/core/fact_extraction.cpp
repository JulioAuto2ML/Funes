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
    "decisions, and things done that change their world (a reminder or job "
    "scheduled or removed for them, a bill paid, a purchase made, something sent "
    "on their behalf).\n\n"
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
    "- Not the assistant's own work: files it saved, code, agents or settings it "
    "wrote or changed, searches it ran, reports or drafts it produced. Those are "
    "kept elsewhere.\n"
    "- Sometimes you are shown a Note instead of an exchange: something stored "
    "earlier, often several past conversations merged, in the assistant's words. "
    "Apply the same rules: keep only what it establishes about the person and "
    "their world, nothing the assistant offered, asked or did for itself.\n"
    "- Each fact is one short self-contained sentence. Name the person (use the "
    "name if a remembered fact gives it, else \"the user\") instead of a pronoun, "
    "and write dates as dates, not \"tomorrow\" or \"next week\".\n"
    "- Write each fact in the language the person used.\n"
    "- kind is one of: about_person, preference, plan, decision, event.\n"
    "- If a new fact makes a remembered fact outdated or wrong, set replaces to "
    "that remembered fact's number. Otherwise null.\n"
    "- Do not restate a fact that is already remembered.";

// Weekday and month names: a fact written in English from a Spanish exchange
// capitalises them without them being in the source.
const std::set<std::string> kCalendar = {
    "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
    "january", "february", "march", "april", "may", "june", "july", "august",
    "september", "october", "november", "december",
};

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int64_t as_id(const json& v) {
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_string()) {
        try { return std::stoll(v.get<std::string>()); } catch (...) {}
    }
    return 0;
}

} // namespace

std::vector<std::string> proper_nouns(const std::string& fact) {
    std::vector<std::string> out;
    std::string word;
    bool sentence_start = true;
    auto flush = [&](bool ends_sentence) {
        if (!word.empty()) {
            if (!sentence_start && word.size() >= 3
                && std::isupper(static_cast<unsigned char>(word[0]))
                && !kCalendar.count(lower_ascii(word)))
                out.push_back(word);
            sentence_start = false;
        }
        if (ends_sentence) sentence_start = true;
        word.clear();
    };
    for (unsigned char c : fact) {
        if (std::isalnum(c) || c >= 0x80 || c == '-') word += static_cast<char>(c);
        else flush(c == '.' || c == '!' || c == '?' || c == ':');
    }
    flush(false);
    return out;
}

std::vector<ChatMessage> extraction_messages(const std::string& user_text,
                                             const std::string& reply,
                                             const std::vector<MemoryStore::Memory>& known,
                                             const std::string& date,
                                             const std::string& kind) {
    std::ostringstream u;
    u << "Already remembered:\n";
    if (known.empty()) u << "(nothing relevant)\n";
    for (const auto& m : known) u << "[" << m.id << "] " << m.presented() << "\n";
    if (kind == "note")
        u << "\nNote (stored " << date << "):\n" << reply << "\n";
    else
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
    // worth accepting: it is the facts list with the wrapper forgotten. An
    // empty object is the other: the 27B answered `{}` for "nothing here" on
    // a copy of yoda's pool, and refusing it left those logs live.
    // And the answer wrapped in a one-element list, `[{"facts": [...]}]`.
    if (j.is_array() && j.size() == 1 && j[0].is_object() && j[0].contains("facts")) j = j[0];
    if (j.is_array()) j = json{{"facts", j}};
    if (j.is_object() && j.empty()) j = json{{"facts", json::array()}};
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
        // A migrated log is not "already remembered" as far as its own facts
        // are concerned — it is what they are being extracted from.
        std::vector<MemoryStore::Memory> known;
        for (auto& m : memory.recall(e.user_id, e.agent, e.user_text, opt.known_k + 1,
                                     /*touch=*/false))
            if (m.id != e.source_memory && static_cast<int>(known.size()) < opt.known_k)
                known.push_back(std::move(m));
        const std::string date = e.created_at.substr(0, 10);

        std::string answer;
        try {
            answer = complete(extraction_messages(e.user_text, e.reply, known, date, e.kind));
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
        // A tainted run's sources, or for a migrated log the log's own
        // origin ("memory:<id>" before 7.1), so a fact can be traced back.
        prov.origin  = e.tainted || e.source_memory > 0 ? e.origin : "session:" + e.session;
        int stored = 0;
        // Grounding (7.3): a fact may only name what its source names, or what
        // this person's memories already do. On a copy of yoda's pool the
        // 27B turned a Ford Territory into a Renault Territory once in two
        // runs — a fluent, confident, wrong fact about the person.
        std::string source = lower_ascii(e.user_text + "\n" + e.reply);
        for (const auto& k : known) source += "\n" + lower_ascii(k.text);

        for (const auto& f : facts) {
            std::string ungrounded;
            for (const auto& noun : proper_nouns(f.text))
                if (source.find(lower_ascii(noun)) == std::string::npos
                    && !memory.mentions(e.user_id, noun)) {
                    ungrounded = noun;
                    break;
                }
            if (!ungrounded.empty()) {
                ++report.ungrounded;
                std::cerr << "[facts] exchange " << e.id << ": \"" << ungrounded
                          << "\" is in neither the exchange nor any memory, skipped: "
                          << f.text << "\n";
                continue;
            }
            try {
                // An update the model could not be trusted to apply (its target
                // was taught, not extracted) is still stored; the link judge's
                // contradicts verdict decides between the two.
                if (!f.claims_replacement) {
                    const auto [near_id, sim] = memory.nearest_live(e.user_id, e.agent, f.text,
                                                                    e.source_memory);
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
        // A migrated exchange log has given up its facts (often none): it
        // leaves recall. Archived, not deleted — unarchive_migrated undoes it.
        if (e.source_memory > 0 && memory.archive(e.user_id, e.source_memory))
            ++report.archived;
        ++report.done;
        report.facts += stored;
    }
    return report;
}

} // namespace funes
