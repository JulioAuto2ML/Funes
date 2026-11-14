// =============================================================================
// src/core/heal.h — what to do when an incident opens (self-healing H2)
// =============================================================================
// docs/self-healing-plan.md, phase H2. A probe opens an incident (probes.h);
// this decides, from a policy an admin writes (config/heal.yaml), what Funes
// does about it — with no model involved:
//
//   observe  nothing beyond the notice (no rule matches)
//   explain  the notice carries what a person should do ("needs sudo: run …")
//   propose  a runbook call is stored as a proposal; "aprobar N" runs it
//   act      the runbook runs at once (an approved proposal, so it is on the
//            same record), and the notice says what was done
//
// A runbook is a tool call some agent already has — sysadmin's
// run_script/restart_unit, operator's run_job_now — so it runs with that
// agent's grants and through dispatch_tool's checks, like any approved
// proposal. Nothing here can do what no agent was given.
//
// Limits that hold whatever the policy says: a rule that would act declares
// `undo:` (H2 accepts only `none` — a restart or a retry has nothing to put
// back; snapshot runbooks come with the first one that writes, through the
// guardian); `act` runs at most twice per incident key in 24 hours and then
// falls back to proposing — a fix that has not held twice is not a fix — and
// at most `max_per_day` times per rule.
// =============================================================================

#pragma once
#include "doctor.h"
#include "incidents.h"
#include "json.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace funes::heal {

enum class Level { Observe = 0, Explain = 1, Propose = 2, Act = 3 };

struct Rule {
    std::string    when;      // an incident key, or a prefix ending in '*'
    Level          level = Level::Observe;
    std::string    action;    // what the call does, for a person: "reiniciar el bridge de Funes"
    std::string    agent;     // propose/act: whose grants the call runs with
    std::string    tool;
    nlohmann::json args = nlohmann::json::object();
    std::string    undo;      // propose/act: "none"
    std::string    explain;   // what a person should know or do
    int            max_per_day = 3;
};

struct Policy {
    std::vector<Rule> rules;
    // What the doctor (doctor.h, H3) may choose from for an incident no rule
    // covers. Same shape as a rule's call; always proposed, never acted.
    std::vector<doctor::Runbook> runbooks;
};

// config/heal.yaml → policy. "" or the first thing wrong with it.
std::string parse_policy(const std::string& yaml_text, Policy& out);

// The first rule whose `when` matches `key`; null when none does.
const Rule* match(const Policy& policy, const std::string& key);

struct Options {
    // Store the rule's call as a proposal; its number, 0 on failure.
    std::function<int64_t(const Rule&, const incidents::Incident&)> propose;
    // Run it now. {ok, what the call answered}.
    std::function<std::pair<bool, std::string>(const Rule&, const incidents::Incident&)> act;
    std::function<int64_t()> now;
    // An incident no rule covers (H3): handed to the doctor, off this thread.
    std::function<void(const incidents::Incident&)> unmatched;
    std::string locale = "es";
};

class Healer {
public:
    Healer(Policy policy, Options opt);
    // Decides and does what the policy says for a newly opened incident, and
    // returns the text to add to its notice ("" for observe).
    std::string on_open(const incidents::Incident& i);
    void set_policy(Policy p) { policy_ = std::move(p); }
    // A copy, taken on the probe thread (the one that calls set_policy).
    std::vector<doctor::Runbook> runbooks() const { return policy_.runbooks; }

private:
    Policy policy_;
    Options opt_;
    std::map<std::string, std::vector<int64_t>> acted_by_key_;
    std::map<std::string, std::vector<int64_t>> acted_by_rule_;
};

} // namespace funes::heal
