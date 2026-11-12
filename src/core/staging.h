// =============================================================================
// src/core/staging.h — host changes run against a copy (OS plan, phase 6)
// =============================================================================
// The approval gate (effects.h, 7.11) stops a tainted run's `host` call and
// asks the person — but "run backup_config with these arguments?" is a
// question about intentions. This answers a better one: what would it change?
//
// A `run_script` call to a script classed `host`, in a tainted run, runs for
// real against an overlay. The `funes _exec` trampoline enters a user + mount
// namespace (no privilege: the process maps its own uid and gets capabilities
// only inside the namespace) and mounts an overlayfs over each directory the
// manifest's `access:` lets it write, lower = the real directory, upper = a
// directory under the stage. The script sees the host and changes it; every
// change lands in the upper layers, which are then the diff. Landlock still
// applies on top, unchanged.
//
// The person then approves what was done, not what was asked for: commit()
// replays the upper layers onto the real directories (refusing if a file it
// would touch changed in the meantime), discard() deletes the stage. Nothing
// re-runs, so a script that would behave differently a second time cannot.
//
// Only directories are staged (overlayfs mounts a directory), and not one
// that holds the stage itself. Writes into the caller's workspace are not
// staged — `workspace` effects need no ceremony — and the network is not
// either: a script that also talks to the world is `external`, not `host`.
// =============================================================================

#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace funes::staging {

// Where stages live (main sets it next to memory.db: a proposal lasts a day
// and should survive a restart; /tmp may be a tmpfs without user xattrs).
void set_root(const std::string& dir);
const std::string& root();

// Tries a staged write through the trampoline once and remembers whether it
// worked. Needs the trampoline and the sandbox on (sandbox.h): staging is a
// promise that nothing else on the host changed, which only holds under
// Landlock. `reason` says why not.
bool probe(std::string& reason);
bool available();
// For tests: pretend probe() said so.
void set_available_for_test(bool on);

// Makes a stage for `roots` (absolute directories). Empty, with `err` set,
// when a root is not a directory, holds or is held by the stage, overlaps
// another root, or has a character overlayfs options cannot carry.
std::string prepare(const std::vector<std::string>& roots, std::string& err);

// The roots a stage was prepared for, in layer order.
std::vector<std::string> roots_of(const std::string& stage);

// In the trampoline, before the sandbox: enter a user + mount namespace and
// mount each root's overlay. Returns an empty string or what failed.
std::string enter(const std::string& stage, const std::vector<std::string>& roots);

struct Change {
    enum Kind { Added, Modified, Deleted, Replaced };   // Replaced: a directory emptied and refilled
    Kind        kind = Added;
    std::string path;      // the real path it applies to
    std::string type;      // file | dir | symlink | other
};

// What the upper layers hold, against the real directories now. Records each
// touched path's current state in the stage, so commit() can tell if it moved.
std::vector<Change> collect(const std::string& stage);

// One line per change ("  modified  /etc/x"), for the model and the person.
std::string summarize(const std::vector<Change>& changes);

// A unified diff of the text files among them, at most `cap` bytes.
std::string diff(const std::string& stage, size_t cap);

// Applies the stage collect() recorded. Empty on success; otherwise what
// stopped it — before anything was written when a path changed since
// collect(), or how far it got when a write failed.
std::string commit(const std::string& stage, size_t* applied = nullptr);

// Deletes a stage (the overlay leaves a work directory without permissions).
void discard(const std::string& stage);

// Removes stages under root() that are not in `keep` and older than
// `min_age_seconds` — a crash between prepare() and a proposal, or a
// proposal that expired undecided.
int prune(const std::set<std::string>& keep, int64_t min_age_seconds);

} // namespace funes::staging
