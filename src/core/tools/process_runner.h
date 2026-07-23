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
#include <vector>

namespace funes::proc {

struct Result {
    int         exit_code = -1;
    std::string output;      // combined stdout+stderr, capped and trimmed to
                             // a clean UTF-8 boundary (not checked for
                             // binary-ness — the caller decides that)
    bool        timed_out = false;
};

// Runs argv[0] with the rest of argv as its literal arguments (no shell).
// cwd is the child's working directory. Killed (whole process group) via
// SIGKILL if still running after timeout_seconds.
Result run_argv(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                int timeout_seconds, size_t max_output_bytes);

// Runs `command` via `/bin/sh -c` — arbitrary shell execution. Same
// timeout/capture semantics as run_argv.
Result run_shell_command(const std::string& command, const std::filesystem::path& cwd,
                         int timeout_seconds, size_t max_output_bytes);

} // namespace funes::proc
