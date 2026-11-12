// =============================================================================
// src/core/observe.h — learn mode: what a child actually touched (OS plan, phase 5)
// =============================================================================
// Nobody writes a minimal sandbox profile by hand, which is why `access:`
// blocks were guessed and no manifest declared `connect:`. So the profile is
// learned: the child runs under its sandbox as usual, and every file it opens,
// creates, removes or executes and every socket it connects is recorded. A
// deterministic fold (propose(), below) turns the record into a candidate
// `access:` block; an admin reads it and commits the manifest. The model is
// not in the enforcement seat — at most it reads the proposal and says what
// looks odd (`funes script-learn x … | funes ask "anything odd?"`).
//
// How it watches, unprivileged: a seccomp *user-notification* filter
// (SECCOMP_RET_USER_NOTIF, Linux 5.0; CONTINUE 5.5). The trampoline forks;
// the child installs the filter, passes its listener fd up, and execs the
// program. Every watched syscall from it and everything it starts stops until
// the supervisor — the trampoline, now the program's parent — has read the
// arguments out of /proc/<pid>/mem and answered CONTINUE. Nothing is decided
// here: the kernel then runs the call exactly as it would have, Landlock
// included, so an access the sandbox refuses is still recorded as *attempted*
// — which is what tells you what the sandbox is missing. Landlock's own audit
// records would be simpler but need ABI 7 (Linux 6.15); yoda runs 6.12.
//
// Observation-only on purpose. Reading a path out of another process's memory
// and then letting the call proceed is a time-of-check/time-of-use race if you
// use it to *decide* anything (a second thread can rewrite the path), which is
// why the enforcement stays with Landlock and this only ever says CONTINUE.
// =============================================================================

#pragma once
#include "sandbox.h"
#include <string>
#include <utility>
#include <vector>

namespace funes::observe {

// One thing the child did. `op`:
//   "read", "write" (an existing file opened for writing), "exec";
//   "create" / "remove" — a directory entry made or taken away (O_CREAT of a
//   new file, mkdir, unlink, rmdir, rename, link): Landlock grants those on
//   the *directory*, so that is where the fold puts them;
//   "connect" — `family` "inet" (with `proto` "tcp"/"udp"/"", `port`, `addr`)
//   or "unix" (with `path`; "@name" for an abstract socket).
// Paths are absolute and symlink-resolved as far as they exist (what Landlock
// judges); a path in the caller's workspace may be written ${WORKSPACE}/…
// (rebase()).
struct Access {
    std::string op;
    std::string path;
    std::string family;
    std::string proto;
    int         port = 0;
    std::string addr;
};

std::string to_json_line(const Access& a);
// Rewrites paths inside `workspace` as ${WORKSPACE}/… — what makes one run's
// record comparable with another account's, and what lets the fold tell the
// caller's own files from somebody else's.
void rebase(std::vector<Access>& seen, const std::string& workspace);
// Lines that do not parse are skipped (the file is ours, but a cap can cut one).
std::vector<Access> parse_lines(const std::string& text);

// Forks, runs argv[0..] (execvp) under a user-notification filter, records
// each distinct access as one JSON line on `out_fd`, and returns the child's
// exit code (128 + signal when it was killed). The caller's own sandbox, if
// any, is inherited — apply it before calling. Returns -1 (and sets `error`)
// when the filter cannot be installed; the program has not run then.
int supervise(char** argv, int out_fd, std::string& error);

// ── the fold ─────────────────────────────────────────────────────────────────

struct Grant {
    std::string kind;    // "read" | "exec" | "write" | "connect"
    std::string value;   // a path with ${VAR}s substituted, or a port number
    std::vector<std::string> evidence;   // up to a few observed paths/addresses
};

struct Proposal {
    std::vector<Grant> needed;    // not covered by the base, the workspace or the manifest
    std::vector<Grant> declared;  // already in the manifest, and used
    std::vector<Grant> unused;    // in the manifest, never touched while observed
    std::vector<std::string> notes;   // unix sockets, UDP, red flags
    size_t observed = 0;
};

struct FoldContext {
    // What every run gets without asking: sandbox::base(), the script's own
    // directory, the workspace root (any account's), the private temp dirs.
    funes::sandbox::Policy implicit;
    std::vector<std::string> implicit_prefixes;   // e.g. "/tmp/funes-exec-"
    // The manifest's `access:` as written (expanded).
    funes::sandbox::Policy declared;
    // Under it, anything but ${WORKSPACE} belongs to another account.
    std::string workspace_root;
    // ${NAME} → value, substituted longest value first (FUNES_ROOT before HOME).
    std::vector<std::pair<std::string, std::string>> vars;
    // Never proposed as a grant in their own right: collapsing stops below
    // them ($HOME, the checkout, /, /tmp …).
    std::vector<std::string> too_broad;
};

// Deterministic: same observations, same proposal. Read/exec of a path that
// does not exist is dropped (interpreters probe dozens of paths that fail
// with ENOENT); create/remove is granted on the directory;
// three or more grants under one directory collapse to it, never past
// `too_broad`; a grant inside another of equal or wider right is dropped.
Proposal propose(const std::vector<Access>& seen, const FoldContext& ctx);

// The proposal as the YAML a manifest takes, with the evidence and the notes
// as comments.
std::string render(const Proposal& p, const std::string& owner);

// Where learned accesses are kept: FUNES_LEARN_DIR, else a `learn` directory
// beside the workspace root (data/learn), one <owner>.jsonl per script or MCP
// server.
std::string log_path(const std::string& owner, const std::string& workspace_root);

// Adds the accesses the log does not have yet; returns how many were new.
// Stops at 4 MB — a log that big is not being read, and a run must not fill
// a disk because a manifest still says `learn: true`.
size_t append_log(const std::string& path, const std::vector<Access>& seen);

// FoldContext for a child: base(), `own_dir` (a script's library directory;
// may be empty), ${WORKSPACE}, /tmp/funes-exec-*, the declared access, and
// FUNES_ROOT / HOME as variables. Anything else under `workspace_root` is
// another account's and is flagged, never granted quietly.
FoldContext context_for(const std::string& own_dir, const std::string& workspace_root,
                        const funes::sandbox::Policy& declared);

} // namespace funes::observe
