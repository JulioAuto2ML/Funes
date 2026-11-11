// =============================================================================
// src/core/tools/shell_tool.cpp — execute_shell native tool
// =============================================================================
// Runs an arbitrary shell command. Since 7.5 the kernel confines it to the
// caller's workspace (Landlock + seccomp, src/core/sandbox.h): it can read the
// system, write the workspace and a private temp directory, and nothing else —
// not funes.local, not memory.db, not another account's files. The other
// guardrails: opt-in (FUNES_ALLOW_SHELL=1, unset by default), a cgroup with
// limits (cgroup.h), a hard timeout, and a capped output size. On a kernel
// without Landlock, or with FUNES_SANDBOX=0, it is unconfined again — the
// tool's description says which, so the model is not told a falsehood.

#include "../funes_config.h"
#include "../text_utils.h"
#include "../tools.h"
#include "fs_guard.h"
#include "process_runner.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_OUTPUT_BYTES     = 16 * 1024;
constexpr int    DEFAULT_TIMEOUT_SECS = 20;
constexpr int    MAX_TIMEOUT_SECS     = 120;

ToolResult execute_shell_handler(const fs::path& default_workspace, const json& args, const ToolContext& ctx) {
    // Per-user, with the agent's own `workspace_dir` applied inside it —
    // same resolution read_file/write_file use, from fs_guard.h — the
    // command's working directory and the root of what the sandbox lets it
    // write.
    fs::path workspace = funes::fsguard::workspace_for(default_workspace, ctx.user_id,
                                                       ctx.workspace_dir);

    if (!funes::shell_allowed())
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

    // Confined to the caller's workspace by the kernel (sandbox.h), not by
    // where the command happens to `cd`.
    funes::proc::Result r = funes::proc::run_shell_command(
        command, workspace, timeout, MAX_OUTPUT_BYTES, std::nullopt,
        funes::sandbox::for_workspace(workspace));

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

    ToolResult out{text, r.timed_out || r.exit_code != 0};
    out.usage = r.usage;
    return out;
}

} // namespace

void register_shell_tool(ToolRegistry& reg, const std::string& workspace_dir) {
    fs::path workspace = workspace_dir;
    std::error_code ec;
    fs::create_directories(workspace, ec);

    reg.add({
        "execute_shell",
        std::string(funes::sandbox::enabled()
            ? "Run a shell command, confined by the kernel to this agent's workspace: it "
              "can read system files and write only inside the workspace (anything else "
              "is 'Permission denied'). "
            : "Run a shell command with the Funes process's own permissions (NOT sandboxed — "
              "a command can access anything that account can). ") +
        "Working directory is this "
        "agent's workspace (default: " + workspace.string() + "; some agents are scoped "
        "to a different folder); output capped at 16 KB; killed after timeout_seconds "
        "(default 20, max 120). Disabled unless the operator has set FUNES_ALLOW_SHELL=1.",
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
