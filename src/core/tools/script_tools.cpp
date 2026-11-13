// =============================================================================
// src/core/tools/script_tools.cpp — list_scripts / run_script
// =============================================================================
// The narrow counterpart to execute_shell. Both start a process; the
// difference is who chose what runs. `execute_shell` takes a command line the
// model wrote, so granting it grants everything the Funes account can do.
// `run_script` takes a *name*, which must appear in the calling agent's
// `scripts:` allowlist and resolve to a manifest in the central library
// (FUNES_SCRIPTS_DIR) that an admin installed. The model supplies arguments,
// never a command, and they arrive as argv tokens — no shell parses them.
//
// So an agent that needs to run one known job no longer needs the switch that
// lets it run any job at all. See core/script_library.h for the model and
// scriptlib/README.md for the manifest format.
//
// Three refusals live here rather than in the library, because they are about
// *this call* rather than about the script: an agent with no grants, a name
// that isn't in the grant list, and a name that is granted but isn't
// installed. Each says which, because "cannot run that" sends a local model
// looking for the wrong fix — usually execute_shell.

#include "../observe.h"
#include "../script_library.h"
#include "../staging.h"
#include "../text_utils.h"
#include "../tools.h"
#include "fs_guard.h"
#include "script_tools.h"
#include "process_runner.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_OUTPUT_BYTES = 16 * 1024;   // as execute_shell

bool granted(const ToolContext& ctx, const std::string& name) {
    // Deny by default: an agent's `scripts:` list is exhaustive, unlike
    // `tools:` where empty means everything. A script added to the library
    // tomorrow must not become runnable by every agent shipped today.
    return std::find(ctx.scripts.begin(), ctx.scripts.end(), name) != ctx.scripts.end();
}

// The agent's grant narrowed by the caller's own permissions. A per-script
// entry ("run_script:backup_workspace": false) denies one script to one
// account through every agent that has it; the agent loop re-checks the same
// key at dispatch, so this list is what the model is *told*, not the boundary.
std::vector<std::string> available(const ToolContext& ctx) {
    std::vector<std::string> out;
    for (const auto& name : ctx.scripts)
        if (ctx.permissions.allows_tool("run_script:" + name)) out.push_back(name);
    return out;
}

std::string name_list(const std::vector<std::string>& names) {
    std::ostringstream oss;
    for (size_t i = 0; i < names.size(); ++i) oss << (i ? ", " : "") << names[i];
    return oss.str();
}

ToolResult list_scripts_handler(const std::string& scripts_dir, const ToolContext& ctx) {
    if (ctx.scripts.empty())
        return {"No scripts are available to this agent. Scripts are granted per "
                "agent in its YAML definition (`scripts:`) and installed by an "
                "administrator in the script library; there is nothing here you can "
                "enable yourself.", false};

    const std::vector<std::string> names = available(ctx);
    if (names.empty())
        return {"This agent is granted " + std::to_string(ctx.scripts.size()) +
                " script(s), but this account's permissions deny all of them. An "
                "administrator can grant them; there is nothing to work around here.",
                false};

    std::vector<funes::ScriptSpec> specs = funes::load_scripts(scripts_dir, names);
    const size_t installed = specs.size();
    specs.erase(std::remove_if(specs.begin(), specs.end(), [&](const funes::ScriptSpec& s) {
        return !funes::script_permitted(ctx.permissions, s); }), specs.end());
    if (specs.empty() && installed > 0)
        return {"This agent's scripts are for administrators, and this account is not one. "
                "An administrator can grant them; there is nothing to work around here.", false};
    if (specs.empty())
        return {"This agent is granted " + std::to_string(names.size()) +
                " script(s) (" + name_list(names) + ") but none of them is installed "
                "in the script library. Tell the user: the grant and the library "
                "disagree, which an administrator has to fix.", true};

    std::ostringstream oss;
    oss << "Scripts you can run with run_script:\n";
    for (const auto& s : specs) {
        oss << "- " << s.name;
        if (!s.description.empty()) oss << ": " << s.description;
        oss << "\n  arguments: " << s.usage() << "\n";
        oss << "  returns: " << s.returns() << "\n";
    }
    return {oss.str(), false};
}

// A run of a `learn: true` script: keep what it touched (observe.h), and say
// on the log when it reached for something its manifest does not grant —
// under enforcement that was a refusal, and the first sign a script, or a
// dependency it pulled in, changed what it does.
void record_learned(const funes::ScriptSpec& spec, const std::string& observed,
                    const fs::path& workspace, const fs::path& workspace_root) {
    auto seen = funes::observe::parse_lines(observed);
    funes::observe::rebase(seen, workspace.string());
    const std::string log = funes::observe::log_path(spec.name, workspace_root.string());
    const size_t added = funes::observe::append_log(log, seen);
    const auto ctx = funes::observe::context_for(fs::path(spec.file).parent_path().string(),
                                                 workspace_root.string(), spec.access);
    const auto p = funes::observe::propose(seen, ctx);
    std::cerr << "[learn] " << spec.name << ": " << seen.size() << " accesses, " << added
              << " new to " << log;
    if (!p.needed.empty()) {
        std::cerr << "; outside its access:";
        for (const auto& g : p.needed) std::cerr << " " << g.kind << " " << g.value;
    }
    for (const auto& n : p.notes)
        if (n.rfind("RED FLAG", 0) == 0) std::cerr << "; " << n;
    std::cerr << "\n";
}

} // namespace

std::string funes::run_script_refusal(const std::string& scripts_dir, const json& args,
                                      const ToolContext& ctx) {
    if (!args.is_object() || !args.contains("name") || !args["name"].is_string()
        || args["name"].get<std::string>().empty())
        return "Missing 'name' argument — run_script takes the name of a script from "
               "the library, not a command line.";
    const std::string name = args["name"].get<std::string>();

    if (ctx.scripts.empty())
        return "This agent has no scripts granted, so '" + name + "' cannot be run. "
               "Scripts are granted per agent in its YAML definition; say so rather "
               "than looking for another way to run it.";
    if (!granted(ctx, name))
        return "Script '" + name + "' is not available to this agent. Available: " +
               name_list(available(ctx)) + ".";

    funes::ScriptSpec spec;
    const std::string load_err = funes::load_script(scripts_dir, name, spec);
    if (!load_err.empty())
        return load_err + " (This agent is allowed to run it, but the script library "
               "does not have it — an administrator has to install it.)";

    if (!funes::script_permitted(ctx.permissions, spec))
        return "Script '" + name + "' is for administrators, and this account is not one. "
               "An administrator can grant it; say so rather than looking for another way.";

    std::vector<std::string> argv;
    return funes::build_argv(
        spec, args.contains("arguments") ? args["arguments"] : json::object(), argv);
}

namespace {

ToolResult run_script_handler(const fs::path& default_workspace, const std::string& scripts_dir,
                              const json& args, const ToolContext& ctx) {
    if (const std::string refusal = funes::run_script_refusal(scripts_dir, args, ctx);
        !refusal.empty())
        return {refusal, true};
    const std::string name = args["name"].get<std::string>();

    funes::ScriptSpec spec;
    funes::load_script(scripts_dir, name, spec);
    std::vector<std::string> argv;
    funes::build_argv(spec, args.contains("arguments") ? args["arguments"] : json::object(), argv);

    // Same working directory every other file-touching tool resolves to, so a
    // script writes where the user's own files are and a second account's
    // run cannot land in the first's folder — and, below, the root of what
    // the sandbox lets it write.
    const fs::path workspace = funes::fsguard::workspace_for(default_workspace, ctx.user_id,
                                                             ctx.workspace_dir);

    // A JSON script's stdout is its contract, so its stderr is kept out of it:
    // one deprecation warning from a library would otherwise fail a schema the
    // script itself satisfied. A text script keeps the merged stream, which is
    // what a person reading a run wants.
    const bool json_out = (spec.output_format == "json");
    // The grant is the sandbox (sandbox.h): the caller's workspace, the
    // script's own library directory, and what its manifest declares.
    funes::sandbox::Policy policy = funes::script_policy(spec, workspace);
    // A staged call (the effects gate, staging.h): the directories the stage
    // was prepared for are overlaid, so the script's writes there land in it.
    if (!ctx.stage_dir.empty()) {
        if (!funes::staging::available())
            return {"Staging is not available here, so '" + name + "' was not run.", true};
        policy.stage = ctx.stage_dir;
        policy.overlay = funes::staging::roots_of(ctx.stage_dir);
    }
    funes::proc::Result r = funes::proc::run_argv(argv, workspace, spec.timeout_seconds,
                                                  MAX_OUTPUT_BYTES, spec.env, json_out,
                                                  spec.limits, policy);
    if (!r.observed.empty()) record_learned(spec, r.observed, workspace, default_workspace);
    // Every answer from here on is about a program that ran, so it carries
    // what that program cost (the [tool] line prints it).
    auto ran = [&r](ToolResult t) { t.usage = r.usage; return t; };

    if (!funes::looks_like_text(r.output))
        r.output = "[" + std::to_string(r.output.size()) +
                   " bytes of output omitted — not valid UTF-8 text]";

    // A script that declares JSON output has its stdout checked against that
    // shape before the model ever sees it — the pipeline-stage contract
    // (pipelines/*.yaml, write_structured) applied to a script, so a stage
    // cannot report success while handing the next stage the wrong shape.
    // Only on a clean exit: a failing script's stderr is the diagnosis, and
    // demanding JSON of it would replace the error with a complaint about the
    // error's format.
    json parsed;
    if (!r.timed_out && r.exit_code == 0) {
        const std::string shape_err = funes::validate_output(spec, r.output, parsed);
        if (!shape_err.empty()) {
            // The script's own stderr goes with the complaint: it is usually
            // the traceback that explains why stdout was wrong, and an admin
            // reading "printed something that is not JSON" needs it.
            std::string text = shape_err;
            if (!r.stderr_output.empty()) text += "\nstderr:\n" + r.stderr_output;
            return ran({text, true});
        }
    }

    // A script whose output is a declared shape returns that shape and nothing
    // else — no "exit_code: 0" banner in front of it. The contract exists so
    // the next step can consume the result (hand it to write_structured, read
    // a field out of it); a prefix would mean every consumer has to strip one,
    // which is the kind of thing exactly one of them will forget.
    if (json_out && !r.timed_out && r.exit_code == 0)
        return ran({parsed.dump(2), false});

    std::string text;
    if (r.timed_out)
        text = "TIMED OUT after " + std::to_string(spec.timeout_seconds) +
               "s (process killed)\n" + r.output;
    else if (r.exit_code == 127)
        // execvp failed: a missing interpreter or a script without the
        // executable bit. Distinguished because it is an installation fault,
        // not a bad argument, and the model would otherwise report it as the
        // script's own failure.
        text = "Script '" + spec.name + "' could not be started (exit 127) — its "
               "interpreter is missing, or the file is not executable. This is an "
               "installation problem, not something the arguments can fix.\n" + r.output;
    else
        text = "exit_code: " + std::to_string(r.exit_code) + "\n" + r.output;

    // Split capture means a failing JSON script's diagnosis lives in the other
    // stream; without this the model gets an exit code and an empty body.
    if (!r.stderr_output.empty() && (r.timed_out || r.exit_code != 0))
        text += "\nstderr:\n" + r.stderr_output;

    return ran({text, r.timed_out || r.exit_code != 0});
}

} // namespace

void register_script_tools(ToolRegistry& reg, const std::string& workspace_dir,
                           const std::string& scripts_dir) {
    fs::path workspace = workspace_dir;
    std::error_code ec;
    fs::create_directories(workspace, ec);

    reg.add({
        "list_scripts",
        "List the scripts this agent is allowed to run, with the arguments each one "
        "takes. Scripts are pre-installed programs in the Funes script library — you "
        "can run the ones listed here and no others.",
        {{"type", "object"}, {"properties", json::object()}},
        [scripts_dir](const json&, const ToolContext& ctx) {
            return list_scripts_handler(scripts_dir, ctx);
        }
    });

    reg.add({
        "run_script",
        "Run one of the scripts this agent is allowed to run (see list_scripts) and "
        "return its exit code and output. You name a script and pass its declared "
        "arguments; you cannot pass a command line, and a script that is not granted "
        "to this agent cannot be run by any wording. Output is capped at 16 KB and the "
        "script is killed at its own timeout.",
        {
            {"type", "object"},
            {"properties", {
                {"name",      {{"type", "string"},
                               {"description", "Name of the script, exactly as list_scripts gives it"}}},
                {"arguments", {{"type", "object"},
                               {"description", "The script's declared parameters as name/value pairs. "
                                               "Omit it for a script that takes none."}}}
            }},
            {"required", json::array({"name"})}
        },
        [workspace, scripts_dir](const json& args, const ToolContext& ctx) {
            return run_script_handler(workspace, scripts_dir, args, ctx);
        }
    });
}
