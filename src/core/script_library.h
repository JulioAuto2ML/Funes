// =============================================================================
// src/core/script_library.h — the central store of runnable scripts
// =============================================================================
// The third thing an agent is allowed to do, after tools and other agents.
//
// Why this exists: the only way to run repo code from an agent used to be
// `execute_shell`, which is unconfined arbitrary execution behind one global
// switch (FUNES_ALLOW_SHELL). Granting it so an agent could run one known
// backup script also granted `curl … | sh`. The two are not the same request,
// and they should not need the same grant.
//
// So a script is declared the way a tool is: it lives in one central
// directory (FUNES_SCRIPTS_DIR, default ./scriptlib), it carries a manifest
// naming its parameters, and an agent reaches it only if its own YAML lists
// it under `scripts:`. The library is outside every workspace, so nothing
// the model drives — read_file, write_file, /api/upload — can read a script,
// edit one, or add one; installing a script is an admin editing the repo,
// the same act as adding an agent.
//
// Three properties worth stating because they are the point:
//
//   1. `scripts:` denies by default. An absent or empty list means *no
//      scripts*, the opposite of `tools:`, where empty means all. A new
//      script dropped into the library must not silently become runnable by
//      eighteen agents; each grant is written down.
//   2. Arguments are argv, never a command line. Declared parameters are
//      passed as `--name=value` tokens straight to execvp — there is no
//      shell, so a value containing `;` or `$(…)` is one argument with
//      punctuation in it, not a second command. This is the difference from
//      `execute_shell` that makes the narrower grant meaningful.
//   3. Secrets stay out of the transcript. A manifest's `env:` block is
//      expanded from the server's environment at load and handed to the
//      child process; the model names the script, never the key.
//
// What this file does NOT do is sandbox. A script runs with the Funes
// process's own permissions — it is repo code an admin installed, trusted the
// same way agents/*.yaml is trusted. The guarantee is about *which* code can
// be started and by whom, not about what that code may then do.
// =============================================================================

#pragma once
#include "json.hpp"
#include <string>
#include <utility>
#include <vector>

namespace funes {

struct ScriptParam {
    std::string name;
    std::string description;
    std::string type = "string";   // "string" | "number" | "boolean"
    bool        required = false;
    // When non-empty, the only accepted values (string params only). Checked
    // before the process starts, so a bad value is a refusal the model can
    // correct rather than a script's own usage error.
    std::vector<std::string> choices;
};

struct ScriptSpec {
    std::string name;          // the manifest's filename stem: [a-z0-9_-]
    std::string description;   // one line, shown to the model
    std::string file;          // resolved absolute path of the executable file
    std::string interpreter;   // "python3", "bash", … empty = exec the file itself
    int         timeout_seconds = 30;
    std::vector<ScriptParam> params;
    // Extra environment for the child process, already expanded from ${VAR}.
    std::vector<std::pair<std::string, std::string>> env;

    // A parameter list rendered for the model: "path (string, required) — …".
    std::string usage() const;
};

// Sanitized script name: [a-z0-9_-] only. A name reaches this from the model
// and becomes a path component, so anything else is dropped rather than
// escaped — and the caller compares the result against what was asked for
// instead of silently running a different script.
std::string safe_script_name(const std::string& raw);

// Loads <dir>/<name>.yaml. Returns an error string (empty on success) rather
// than throwing: every caller is a tool handler whose job is to hand the
// model a sentence it can act on.
std::string load_script(const std::string& dir, const std::string& name, ScriptSpec& out);

// Names of every *.yaml in the library, sorted. Used to tell an admin's agent
// what exists, never to decide what may run.
std::vector<std::string> list_script_names(const std::string& dir);

// Loads each name in `allowed` that the library actually has. A grant naming
// a script that isn't installed is skipped, not an error: an agent YAML and
// the library are edited separately, and one stale line must not take the
// other scripts down with it.
std::vector<ScriptSpec> load_scripts(const std::string& dir,
                                     const std::vector<std::string>& allowed);

// Turns a tool call's arguments into the child's argv:
// [interpreter,] <file>, then one `--name=value` token per supplied
// parameter (a true boolean is a bare `--name`; a false one is omitted).
// Returns an error string — unknown parameter, missing required one, wrong
// type, value outside `choices` — in which case `argv` is left untouched.
// Nothing is started until every argument is accounted for.
std::string build_argv(const ScriptSpec& spec, const nlohmann::json& args,
                       std::vector<std::string>& argv);

} // namespace funes
