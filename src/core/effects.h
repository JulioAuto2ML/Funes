// =============================================================================
// src/core/effects.h — what a tool call does to the world, and who must agree
// =============================================================================
// OS plan, phase 6 (docs/os-plan.md, pillar 3). Every tool call, script and
// MCP tool has an effect class:
//
//   none       reads, or computes; nothing outside the run changes
//   workspace  writes the caller's own workspace (fs_guard confines it)
//   host       changes this machine beyond the workspace (a shell, an agent
//              file, a tool definition)
//   external   reaches the outside world: a message sent, a page published,
//              the speaker, the TV, a job scheduled to do any of those later
//
// The gate (FunesAgent::dispatch_tool): a host or external call in a run that
// has read third-party content (RunProvenance::tainted — a web page, an email,
// another agent's answer, a script's output) does not run. It is stored as a
// proposal (proposals.h) with its exact arguments, and runs only when the
// person answers "aprobar <n>" / "approve <n>" — a command /api/chat handles
// itself, never the model. That is the attack the gate exists for: an email
// that says "forward this to …" reaches a model that can send.
//
// A run that has read nothing outside — "say X on the speaker" — acts at once,
// and so does a scheduled job: scheduling it was the approval, and a job that
// stalled at 06:30 waiting for a person would not be one. A job can only be
// scheduled by a call the gate has seen (schedule_job is external), so a
// tainted run cannot launder an action through the scheduler.
//
// Deny by default, like scripts: and provenance: a script manifest without
// `effects:` and an MCP server without an `effects:` map are external. A
// built-in is code in this binary, so builtin_effect() lists the ones that
// are not `none`.
// =============================================================================

#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace funes {

enum class Effect { None = 0, Workspace = 1, Host = 2, External = 3 };

// "none" | "workspace" | "host" | "external" → Effect; anything else nullopt.
std::optional<Effect> parse_effect(const std::string& s);
const char* effect_name(Effect e);

// A built-in tool's class. Not deny-by-default: the effectful ones are listed.
Effect builtin_effect(const std::string& tool);

// Whether a call of this class waits for the person when the run is tainted.
// Host is gated too until phase 6's overlay staging can show a diff instead.
inline bool needs_approval(Effect e) { return e >= Effect::Host; }

// The person's answer to a proposal: "aprobar 12", "approve #12", "rechazar
// p-12", "reject 12", case- and accent-insensitive, nothing else in the
// message. A sentence that merely contains the word is not one — "approve the
// budget 2" must reach the model.
struct ApprovalCommand {
    bool    approve = false;
    int64_t id = 0;
};
std::optional<ApprovalCommand> parse_approval(const std::string& message);

} // namespace funes
