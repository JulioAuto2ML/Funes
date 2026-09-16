// =============================================================================
// src/core/tools/process_runner.h — fork/exec with timeout + output capture
// =============================================================================
// Shared by execute_shell (arbitrary /bin/sh -c command) and read_file's PDF
// extraction (a fixed pdftotext invocation, no shell involved — argv is
// passed straight to execvp so there's no injection surface). One
// implementation of the fork/pipe/poll/timeout dance so it can't drift
// between the two call sites.

#pragma once
#include <filesystem>
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
};

// Runs argv[0] with the rest of argv as its literal arguments (no shell).
// cwd is the child's working directory. Killed (whole process group) via
// SIGKILL if still running after timeout_seconds.
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
                bool separate_stderr = false);

// Runs `command` via `/bin/sh -c` — arbitrary shell execution. Same
// timeout/capture semantics as run_argv.
Result run_shell_command(const std::string& command, const std::filesystem::path& cwd,
                         int timeout_seconds, size_t max_output_bytes);

} // namespace funes::proc
