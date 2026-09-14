// =============================================================================
// src/core/agent_roster.h — what an orchestrator is told about other agents
// =============================================================================
// `funes` learns the agents it may delegate to from a roster injected into its
// system prompt at request time. Until now that roster was every loaded agent,
// unfiltered — which was correct when every account could use every agent, and
// became a bug the moment one could not.
//
// An unfiltered roster fails in a specific, expensive way. The model reads
// "gmail-assistant: reads and drafts your mail", delegates to it, and gets back
// a refusal. It now has to explain a failure whose cause is not in its context,
// so it improvises: it blames a configuration, tells the person to set an
// environment variable, or simply tries again. That is the same failure the
// withheld-tools block in agent.cpp was written for — a model that cannot see
// *why* something is unavailable will invent a reason — and this is the agent
// half of it, which was missed.
//
// So the roster splits in two. Available agents are offered for delegation.
// Denied ones are named with the reason they are denied, which is not decoration
// either: there are two reasons, and they call for opposite advice.
//
//   * Not on this account's allowlist — an admin can grant it. "Ask an
//     administrator" is useful advice.
//   * Backed by a shared identity (AgentConfig::shared_identity) — the agent
//     authenticates as the installation, not as the caller, so granting it
//     would hand over somebody else's mailbox or phone. Here "ask an
//     administrator" is bad advice: the answer is that this is not an account
//     permission at all.
//
// Split into its own header because the data lives in FunesApi (the agent
// table) while the decision is the permission layer's, and a pure function
// over both is the only part worth testing — which the API class is not.

#pragma once
#include "permissions.h"
#include <string>
#include <vector>

namespace funes {

// One loaded agent, as much of it as the roster cares about.
struct RosterEntry {
    std::string name;
    std::string description;
    std::string delegation_notes;
    std::string shared_identity;   // empty = authenticates as the caller
};

struct Roster {
    // "- name: description\n" (+ "  Note: …") per agent this caller may use.
    std::string available;
    // "- name — reason\n" per agent it may not. Empty when there are none,
    // which is the admin case and the single-user case: no block is added to
    // the prompt at all, so nothing is spent saying "you can use everything".
    std::string denied;
};

// `exclude` drops the calling agent from its own roster.
Roster build_roster(const std::vector<RosterEntry>& entries,
                    const Permissions& perms,
                    const std::string& exclude);

// The prompt block for the denied half, or "" when there is nothing to say.
// Worded to stop the two failures seen in practice: blaming a server setting,
// and telling a member to ask an admin for something no admin can grant.
std::string denied_agents_block(const std::string& denied);

// What delegate_to_agent answers when the target is refused. Carries the same
// distinction, because this string is often the only thing the model relays.
std::string delegation_refusal(const std::string& agent,
                               const std::string& shared_identity);

} // namespace funes
