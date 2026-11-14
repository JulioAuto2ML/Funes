// =============================================================================
// src/core/doctor.h — a model's diagnosis, as a choice from a closed list
// =============================================================================
// Self-healing plan, phase H3 (docs/self-healing-plan.md). For an incident no
// rule in config/heal.yaml covers, the doctor asks the model one question with
// a closed set of answers: which runbook from the catalog (heal.yaml
// `runbooks:`), or `explain`. The model writes no command and calls no tool:
// the code gathers the evidence (the incident, what changed on the machine
// before it) and the model only classifies it — picking from a handful of
// options with the evidence in front of it is the part small models do well.
//
// What it picks is only ever *proposed* («aprobar N»); level 3 (act) for a
// doctor's choice is H4, and needs an eval score (tests/heal_eval). An answer
// that does not parse, or names a runbook not in the catalog, is `explain`
// with the model's text dropped: a wrong verb never reaches the proposal
// store.
// =============================================================================

#pragma once
#include "json.hpp"
#include "llm_client.h"
#include <functional>
#include <string>
#include <vector>

namespace funes::doctor {

struct Runbook {
    std::string    id;       // [a-z0-9_-], what the model answers with
    std::string    action;   // for a person: "reiniciar el bridge de Funes"
    std::string    agent;    // whose grants the call runs with
    std::string    tool;
    nlohmann::json args = nlohmann::json::object();
};

struct Case {
    std::string key;
    std::string summary;
    std::string evidence;         // log text: hostile, quoted
    std::string recent_changes;   // host_changes over the hours before, as text
};

struct Verdict {
    std::string choice;      // a runbook id, or "explain"
    std::string diagnosis;   // one or two sentences
    std::string steps;       // what a person should do (explain), may be empty
};

constexpr size_t kMaxEvidenceChars = 1500;
constexpr size_t kMaxChangesChars = 2500;

std::vector<ChatMessage> messages(const Case& c, const std::vector<Runbook>& runbooks);

// The model's answer → verdict. "" on success; otherwise why it was refused,
// and `out` is a bare explain with no model text in it.
std::string parse_verdict(const std::string& answer, const std::vector<Runbook>& runbooks,
                          Verdict& out);

using CompleteFn = std::function<std::string(const std::vector<ChatMessage>&)>;

// One diagnosis; the same function the server and tests/heal_eval call.
Verdict diagnose(const CompleteFn& complete, const Case& c, const std::vector<Runbook>& runbooks,
                 std::string* refusal = nullptr);

// The WhatsApp text: diagnosis, and either the proposal to approve or the
// steps. `proposal` 0 for explain.
std::string verdict_text(const Case& c, const Verdict& v, const Runbook* chosen,
                         int64_t proposal, const std::string& locale);

} // namespace funes::doctor
