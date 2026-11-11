// =============================================================================
// src/core/tools/process_runner.h — fork/exec with timeout + output capture
// =============================================================================
// Shared by execute_shell (arbitrary /bin/sh -c command) and read_file's PDF
// extraction (a fixed pdftotext invocation, no shell involved — argv is
// passed straight to execvp so there's no injection surface). One
// implementation of the fork/pipe/poll/timeout dance so it can't drift
// between the two call sites.

#pragma once
#include "../cgroup.h"
#include "../sandbox.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace funes::proc {

struct Result {
    int         exit_code = -1;
    std::string output;      // stdout, or stdout+stderr combined unless the
                             // caller asked for them apart; capped and trimmed
                             // to a clean UTF-8 boundary (not checked for
                             // binary-ness — the caller decides that)
    std::string stderr_output;  // only ever set when separate_stderr was asked
                                // for; empty otherwise, including when the
                                // child wrote to stderr and it landed in
                                // `output` alongside stdout
    bool        timed_out = false;
    // What the child cost, when it ran in a cgroup of its own (cgroup.h).
    std::optional<funes::cgroup::Usage> usage;
};

// Runs argv[0] with the rest of argv as its literal arguments (no shell).
// cwd is the child's working directory. Killed (whole process group, and
// its whole cgroup when it has one) via SIGKILL if still running after
// timeout_seconds.
//
// `limits` are the child's cgroup limits; nullopt means
// cgroup::default_limits(). Ignored when cgroups are off.
//
// `policy` confines the child with Landlock + seccomp (sandbox.h) — what it
// may read, write and execute, enforced by the kernel. nullopt runs it
// unconfined, which only trusted pipeline code should ask for (src/julio's
// publish step); every model-reachable path passes one. Ignored when the
// sandbox is unavailable or FUNES_SANDBOX=0.
//
// `extra_env` adds to (or overrides in) the environment the child inherits —
// how a script library manifest hands a script its API key without the key
// passing through the model's context (see core/script_library.h). The
// merged block is built in the parent, before the fork, so the child does
// nothing between fork and exec but assign a pointer and call execvp.
// `separate_stderr` splits the child's two streams instead of merging them.
// Combined output is the right default — for a person reading a shell
// command's result, interleaved is how it happened. It is wrong for a program
// whose stdout is a *contract*: a script declaring JSON output (see
// core/script_library.h) would fail its own schema the first time a library
// printed a deprecation warning, which is a brittle contract, not a strict one.
Result run_argv(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                int timeout_seconds, size_t max_output_bytes,
                const std::vector<std::pair<std::string, std::string>>& extra_env = {},
                bool separate_stderr = false,
                const std::optional<funes::cgroup::Limits>& limits = std::nullopt,
                const std::optional<funes::sandbox::Policy>& policy = std::nullopt);

// Runs `command` via `/bin/sh -c` — arbitrary shell execution. Same
// timeout/capture semantics as run_argv.
Result run_shell_command(const std::string& command, const std::filesystem::path& cwd,
                         int timeout_seconds, size_t max_output_bytes,
                         const std::optional<funes::cgroup::Limits>& limits = std::nullopt,
                         const std::optional<funes::sandbox::Policy>& policy = std::nullopt);

// A child prepared for the trampoline, for a caller that forks it itself
// (the MCP stdio client, which keeps its server running across calls): its
// own cgroup, its sandbox, its own temp directory. run_argv goes through the
// same function, so the two cannot drift.
struct Wrapped {
    std::vector<std::string> argv;   // exec this — argv itself when nothing wraps it
    std::string tmpdir;              // point TMPDIR here; empty when there is none
    std::unique_ptr<funes::cgroup::Scope> scope;

    // Kills whatever is left in the cgroup, removes it and the temp directory,
    // and returns what the child cost (nullopt without a cgroup). Idempotent.
    std::optional<funes::cgroup::Usage> finish();
    ~Wrapped() { finish(); }
};
std::unique_ptr<Wrapped> wrap(const std::vector<std::string>& argv,
                              const std::optional<funes::cgroup::Limits>& limits,
                              const std::optional<funes::sandbox::Policy>& policy);

// Told about every child that ran in a cgroup of its own, once it is gone:
// its command line (before wrapping) and what it cost. main() points it at
// the machine collector (host_collector.h), which keeps a baseline per
// program. Set once at startup, before any child runs; it must be cheap and
// thread-safe — it runs on the thread that waited for the child.
using UsageObserver = std::function<void(const std::vector<std::string>& argv,
                                         const funes::cgroup::Usage& usage)>;
void set_usage_observer(UsageObserver observer);
void notify_usage(const std::vector<std::string>& argv, const funes::cgroup::Usage& usage);

// The program that wraps every child once cgroups or the sandbox are on: main() sets it to
// /proc/self/exe — the running binary even after a deploy has replaced the
// file — and hands `funes _exec` to trampoline_main before anything else.
// Unset (every unit test but test_cgroup), children are exec'd directly.
void set_trampoline(const std::string& exe);

// `funes _exec [--cgroup=DIR] [--sandbox --ro= --rx= --rw= --connect=] --
// PROGRAM ARGS...`: write this process into DIR/cgroup.procs, apply the
// sandbox policy, then execvp PROGRAM. Returns only on failure — 126 if it
// could not join or confine itself (it does not run the program outside the
// limits it was given), 127 if PROGRAM could not be exec'd, matching the exit codes a
// direct exec failure has always had.
int trampoline_main(int argc, char** argv);

// The environment a child process gets. An *allowlist* over the server's own
// environment, not a copy of it, because until 7.8 funes_config.h loaded
// config/funes.local into the process environment at startup: every secret the
// operator wrote down — FUNES_SERVICE_TOKEN, FUNES_LLM_KEY, an IMAP password,
// a Tavily key — used to reach every script, every `execute_shell` command (`env` printed
// them into the transcript) and every MCP stdio server, which is somebody
// else's code. Nothing a model can start should be handed the credential
// that authenticates the model's own server. Since 7.8 config files are read
// into a private map instead (funes_config.h), so the environment holds no
// secrets and this allowlist is the second wall, not the only one.
//
// What passes: the variables a program needs to run at all (PATH, HOME, the
// locale and timezone, a temp dir, proxies, the Python/CA plumbing — see the
// list in process_runner.cpp), plus whatever the operator names in
// FUNES_CHILD_ENV (comma-separated), for the one deployment that really does
// want a child to read a credential from the environment. Everything else,
// FUNES_* included, is dropped. `extra_env` (a script manifest's `env:`, an
// MCP server's `env:`) is added last and wins — that is the intended way to
// hand one program one secret.
//
// Returned as "KEY=value" strings: the caller builds envp from them in the
// parent, so the child does nothing between fork and exec but exec.
std::vector<std::string> child_environment(
    const std::vector<std::pair<std::string, std::string>>& extra_env = {});

} // namespace funes::proc
