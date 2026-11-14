// =============================================================================
// src/core/guardian.h — asking the guardian before Funes changes itself
// =============================================================================
// Self-healing plan, phase H1 (docs/self-healing-plan.md). The guardian is a
// separate program (scripts/funes_guardian.py, run every minute by its own
// systemd timer) that snapshots what a change may touch, watches the change
// for a while, and puts the snapshot back if Funes stops answering or an
// incident opens — or when a person says "deshacer N". It has to be separate:
// if a change breaks Funes, Funes cannot be what undoes it.
//
// This is the server's side of it. Today one tool changes the installation
// while it runs — create_agent (agent-doctor rewrites agents/*.yaml live) —
// and it goes through here: begin() before the write, applied() after it.
// When the guardian is configured and begin() fails, the change is refused:
// a change the guardian cannot undo is the thing it exists to prevent.
//
// The guardian runs as a plain child (no sandbox: restoring the checkout and
// restarting units is its job), with only the configuration it needs passed
// explicitly — the child environment allowlist still applies.
// =============================================================================

#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace funes::guardian {

// The guardian script and the checkout it protects. Empty script = off (a
// development checkout without it, or FUNES_GUARDIAN=off).
void configure(const std::string& script, const std::string& root);
bool enabled();

struct Result {
    bool        ok = false;
    int64_t     id = 0;          // begin(): the change's number
    std::string message;         // undo(): what to tell the person; else the error
};

// Snapshot `paths` (inside the checkout) before changing them. `restart` names
// the user units to restart when the change is undone ("funes" for anything
// the server reads at startup or reload).
Result begin(const std::string& reason, const std::string& actor,
             const std::vector<std::string>& paths, const std::vector<std::string>& restart);
// The change is made: watch it for `confirm_after` seconds.
Result applied(int64_t id, int confirm_after_seconds = 900);
// Put change `id` back.
Result undo(int64_t id, const std::string& by);

// "deshacer 3" / "undo #3" alone in a message → 3. Like parse_approval
// (effects.h): answered by /api/chat itself, never by a model.
std::optional<int64_t> parse_undo(const std::string& message);

} // namespace funes::guardian
