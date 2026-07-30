// =============================================================================
// src/core/answer_schema.h — typed final answers
// =============================================================================
// The completion contract (completion_contract.h) checks that the work
// happened. It says nothing about the shape of what comes back. An agent whose
// answer feeds another agent — a specialist reporting to an orchestrator, a
// research step feeding the newsletter pipeline — can return prose that is
// wrong for its consumer and still count as a success.
//
// An answer schema closes that half: the agent declares the JSON its final
// answer must be, and the loop refuses to accept anything else. Same machinery
// as the tool contract — a nudge naming the specific violation, the same nudge
// budget, an explicit failure when the budget is spent. Unlike a tool nudge it
// does NOT force a tool call: we want text from the model, just correct text.
//
// Validation is a deliberate subset of JSON Schema — type, required,
// properties, items, enum, minItems/maxItems. Not a dependency, not a
// standards-compliance project: an agent contract is a handful of fields, and
// the error strings are read by a 7B local model, so they are written for that
// reader rather than for a spec. Anything unrecognised in the schema is
// ignored rather than rejected, so a richer schema degrades to the subset
// instead of failing shut.

#pragma once
#include "json.hpp"
#include <string>

namespace funes {

// Pulls the JSON value out of a model's final answer: a raw parse first, then
// the first fenced ```json block (local models fence habitually, and some
// prepend a sentence no amount of prompting removes). Returns false if there
// is no parseable JSON in `answer` at all.
bool extract_answer_json(const std::string& answer, nlohmann::json& out);

// Empty string = valid. Otherwise one concrete, path-qualified violation
// ("sources[1]: expected string, got number") — the first one found, because
// a list of five complaints makes a small model rewrite everything at once
// and reliably break something that was already correct.
std::string validate_answer(const nlohmann::json& schema, const nlohmann::json& value);

// Appended to the system prompt at config load, so the prompt and the contract
// can't drift apart: whatever the schema says is what the model is told.
std::string schema_prompt_block(const nlohmann::json& schema);

// Injected as a user turn when the answer doesn't validate.
std::string schema_nudge(const std::string& error, const nlohmann::json& schema);

// Final answer when the nudge budget is spent. Like contract_failure, it has
// to read as a failure to a human *and* to a delegating agent that sees only
// this string.
std::string schema_failure(const std::string& error, const std::string& model_answer);

} // namespace funes
