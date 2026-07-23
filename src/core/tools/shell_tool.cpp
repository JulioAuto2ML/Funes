// =============================================================================
// src/core/tools/shell_tool.cpp — execute_shell native tool
// =============================================================================
// Runs an arbitrary shell command with the process's own privileges — this is
// real, unsandboxed code execution, not confined to the workspace the way
// read_file/write_file are (a command can `cd` anywhere the OS user can).
// The only guardrails are: opt-in (FUNES_ALLOW_SHELL=1, unset by default),
// a working directory of the workspace, a hard timeout that SIGKILLs the
// process group, and a capped output size. Only enable this for a Funes
// instance you trust with full access to your account.

#include "../text_utils.h"
#include "../tools.h"
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_OUTPUT_BYTES     = 16 * 1024;
constexpr int    DEFAULT_TIMEOUT_SECS = 20;
constexpr int    MAX_TIMEOUT_SECS     = 120;

struct ShellResult {
    int         exit_code = -1;
    std::string output;
    bool        timed_out = false;
};

// Runs `command` via /bin/sh -c, cwd = `cwd`, killing it (and its process
// group) if it's still running after `timeout_seconds`. Output is stdout+
// stderr combined, capped at MAX_OUTPUT_BYTES.
ShellResult run_shell(const std::string& command, const fs::path& cwd, int timeout_seconds) {
    ShellResult result;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        result.output = "pipe() failed";
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        result.output = "fork() failed";
        return result;
    }

    if (pid == 0) {
        // Child: own process group so a timeout can kill the whole tree,
        // stdout+stderr both go to the pipe, cwd is the workspace.
        setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        if (::chdir(cwd.c_str()) != 0) _exit(126);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);  // execl only returns on failure
    }

    // Parent.
    setpgid(pid, pid);  // avoid a race with the child's own setpgid call
    close(pipefd[1]);
    const int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    auto drain = [&] {
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            if (result.output.size() < MAX_OUTPUT_BYTES)
                result.output.append(buf, static_cast<size_t>(n));
        }
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    int status = 0;
    while (true) {
        drain();

        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;

        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0)) {
            result.timed_out = true;
            kill(-pid, SIGKILL);  // negative pid → whole process group
            waitpid(pid, &status, 0);
            break;
        }

        struct pollfd pfd{pipefd[0], POLLIN, 0};
        const int wait_ms = static_cast<int>(std::min<std::chrono::milliseconds::rep>(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count(), 200));
        poll(&pfd, 1, std::max(wait_ms, 0));
    }

    drain();
    close(pipefd[0]);

    if (!result.timed_out && WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    if (result.output.size() >= MAX_OUTPUT_BYTES) {
        // The byte cap above can land mid-character; trim back to a clean
        // UTF-8 boundary before the caller ever sees this text.
        funes::truncate_utf8_safe(result.output, MAX_OUTPUT_BYTES);
        result.output += "\n[output truncated at 16 KB]";
    }
    return result;
}

bool shell_enabled() {
    const char* v = std::getenv("FUNES_ALLOW_SHELL");
    return v && *v == '1';
}

ToolResult execute_shell_handler(const fs::path& workspace, const json& args, const ToolContext&) {
    if (!shell_enabled())
        return {"Shell execution is disabled. Set FUNES_ALLOW_SHELL=1 to enable it — "
                "commands then run with the Funes process's own permissions inside " +
                workspace.string() + ", with a timeout. Only turn this on if you trust "
                "whoever/whatever can reach this agent.", true};

    if (!args.contains("command") || !args["command"].is_string()
        || args["command"].get<std::string>().empty())
        return {"Missing 'command' argument", true};
    const std::string command = args["command"].get<std::string>();

    int timeout = args.value("timeout_seconds", DEFAULT_TIMEOUT_SECS);
    if (timeout < 1) timeout = 1;
    if (timeout > MAX_TIMEOUT_SECS) timeout = MAX_TIMEOUT_SECS;

    ShellResult r = run_shell(command, workspace, timeout);

    // A command's stdout/stderr is arbitrary bytes, not guaranteed text
    // (e.g. `cat` on a binary file) — refuse to pass that through raw the
    // same way read_file/web_fetch do, rather than risk it downstream.
    if (!funes::looks_like_text(r.output))
        r.output = "[" + std::to_string(r.output.size()) +
                   " bytes of output omitted — not valid UTF-8 text]";

    std::string text;
    if (r.timed_out)
        text = "TIMED OUT after " + std::to_string(timeout) + "s (process killed)\n" + r.output;
    else
        text = "exit_code: " + std::to_string(r.exit_code) + "\n" + r.output;

    return {text, r.timed_out || r.exit_code != 0};
}

} // namespace

void register_shell_tool(ToolRegistry& reg, const std::string& workspace_dir) {
    fs::path workspace = workspace_dir;
    std::error_code ec;
    fs::create_directories(workspace, ec);

    reg.add({
        "execute_shell",
        "Run a shell command with the Funes process's own permissions (NOT sandboxed — "
        "a command can access anything that account can). Working directory is the "
        "workspace (" + workspace.string() + "); output capped at 16 KB; killed after "
        "timeout_seconds (default 20, max 120). Disabled unless the operator has set "
        "FUNES_ALLOW_SHELL=1.",
        {
            {"type", "object"},
            {"properties", {
                {"command",         {{"type", "string"}, {"description", "The shell command to run"}}},
                {"timeout_seconds", {{"type", "integer"},
                                     {"description", "Max seconds before the command is killed (default 20, max 120)"}}}
            }},
            {"required", json::array({"command"})}
        },
        [workspace](const json& args, const ToolContext& ctx) {
            return execute_shell_handler(workspace, args, ctx);
        }
    });
}
