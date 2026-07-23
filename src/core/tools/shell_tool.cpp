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
#include "process_runner.h"
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_OUTPUT_BYTES     = 16 * 1024;
constexpr int    DEFAULT_TIMEOUT_SECS = 20;
constexpr int    MAX_TIMEOUT_SECS     = 120;

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

    funes::proc::Result r = funes::proc::run_shell_command(command, workspace, timeout, MAX_OUTPUT_BYTES);

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
