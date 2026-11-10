// =============================================================================
// src/core/fact_extraction.h — facts instead of exchanges (memory plan, phase 4)
// =============================================================================
// Until 7.2 every exchange a person had with Funes became a memory:
// `User said: "…" — I replied: "…"`. On yoda that was 75% of the pool, and the
// five memories recalled most were two chats about memory, two attachments and
// "I don't have your name recorded" — recalled beside the memory holding the
// name. The transcript is already in `turns`; a memory should be a claim about
// the world, one per row.
//
// Now the exchange is queued (MemoryStore::enqueue_exchange) and this pass,
// run in the background behind busy_gate like the link backfill, asks the
// model one question per exchange: which durable facts did it establish? The
// answer is JSON checked against kFactSchema; an empty list is the common,
// correct answer. Each fact is stored as source `extracted`, carrying the
// exchange's provenance (a run that read a page yields tainted facts).
//
// Deterministic over agentic: queueing, bounding, provenance and storage are
// code. The model is asked only the one thing code cannot answer.

#pragma once
#include "llm_client.h"
#include "memory.h"
#include "json.hpp"
#include <functional>
#include <string>
#include <vector>

namespace funes {

struct ExtractedFact {
    std::string text;
    std::string kind;          // about_person | preference | plan | decision | event
    int64_t     replaces = 0;  // id of a remembered fact this one makes outdated
    // The model said this fact replaces something, whether or not that
    // something was one it may retire. Such a fact is an update, never a
    // restatement: "runs on Monday" beside "runs on Sunday" is 0.97 similar
    // and must not be dropped as saying the same thing.
    bool        claims_replacement = false;
};

// The contract the model's answer is validated against.
const nlohmann::json& fact_schema();

// The two messages for one exchange. `known` are the person's memories most
// similar to it, shown with their ids so the model can say which one a new
// fact replaces and avoid restating one. `date` is the exchange's (YYYY-MM-DD),
// so "tomorrow" can be written as a date.
std::vector<ChatMessage> extraction_messages(const std::string& user_text,
                                             const std::string& reply,
                                             const std::vector<MemoryStore::Memory>& known,
                                             const std::string& date);

// Parses and validates a model answer. A `replaces` naming anything but one of
// `known` is dropped (set to 0), not trusted: the model may only retire a
// memory it was shown, and only an `extracted` or `auto` one. Facts longer
// than kMaxFactBytes are shortened.
bool parse_facts(const std::string& answer, const std::vector<MemoryStore::Memory>& known,
                 std::vector<ExtractedFact>& out, std::string& error);

constexpr size_t kMaxFactBytes = 500;

using CompleteFn = std::function<std::string(const std::vector<ChatMessage>&)>;

struct FactExtractionOptions {
    size_t max_items = 20;   // exchanges per run
    int    known_k   = 6;    // remembered facts shown alongside each exchange
    int    min_age_seconds = 0;   // see MemoryStore::pending_exchanges
    // A fact this similar to a live memory, and replacing nothing, is a
    // restatement and is not stored. Measured on tests/memory_eval: an 8B
    // model paraphrases what it was shown ("Julio takes mate" beside "Julio
    // drinks mate every morning…"), and each paraphrase took a recall slot.
    double restatement_similarity = 0.85;
};

struct FactExtractionReport {
    int seen   = 0;
    int done   = 0;   // answered usably (facts may have been 0)
    int failed = 0;   // the call threw or the answer did not validate
    int facts  = 0;   // stored
    int replaced = 0; // older memories superseded by a fact (phase 5)
    int restated = 0; // facts dropped as restatements of a live memory
    int archived = 0; // migrated exchange logs retired after extraction (phase 6)
};

// Works through the queue, oldest first. `complete` is the model call; the
// caller is responsible for waiting until the machine is quiet before each
// one (main.cpp does, with busy_gate). Safe to call from a background thread.
FactExtractionReport run_fact_extraction(MemoryStore& memory, const CompleteFn& complete,
                                         const FactExtractionOptions& opt = {});

} // namespace funes
