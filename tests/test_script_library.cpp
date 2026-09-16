// =============================================================================
// tests/test_script_library.cpp — the per-agent script allowlist
// =============================================================================
// Two halves, and the first is the one that matters: a script runs only if the
// calling agent was granted it. Everything else here (manifest parsing, argv
// construction, the refusals) exists to keep that grant meaning what it says —
// a script that could be reached under a different name, or an argument that
// could become a second command, would make the allowlist decorative.

#include "agent_config.h"
#include "script_library.h"
#include "tools.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

fs::path lib_dir() { return fs::temp_directory_path() / "funes_test_scriptlib"; }

void write_file(const fs::path& p, const std::string& content, bool executable = false) {
    std::ofstream(p) << content;
    if (executable)
        fs::permissions(p, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace);
}

// A library with one echoing script, one that needs an interpreter, and one
// manifest that points at a file nobody installed.
void build_library() {
    const fs::path dir = lib_dir();
    fs::remove_all(dir);
    fs::create_directories(dir);

    write_file(dir / "greet.sh",
               "#!/bin/sh\n"
               "echo \"args:$*\"\n"
               "echo \"env:${GREET_SECRET:-none}\"\n"
               "echo \"cwd:$(pwd)\"\n",
               /*executable=*/true);
    write_file(dir / "greet.yaml",
               "description: Echo its arguments.\n"
               "run: greet.sh\n"
               "timeout_seconds: 10\n"
               "env:\n"
               "  GREET_SECRET: \"${FUNES_TEST_SCRIPT_SECRET}\"\n"
               "params:\n"
               "  - name: who\n"
               "    type: string\n"
               "    required: true\n"
               "  - name: times\n"
               "    type: number\n"
               "  - name: loud\n"
               "    type: boolean\n"
               "  - name: mode\n"
               "    type: string\n"
               "    choices: [fast, slow]\n");

    write_file(dir / "slow.sh", "#!/bin/sh\nsleep 5\n", true);
    write_file(dir / "slow.yaml",
               "description: Sleeps past its budget.\nrun: slow.sh\ntimeout_seconds: 1\n");

    write_file(dir / "ghost.yaml", "description: Points at nothing.\nrun: ghost.sh\n");

    // Reachable through the filesystem, granted to nobody.
    write_file(dir / "secret.sh", "#!/bin/sh\necho ran-the-ungranted-script\n", true);
    write_file(dir / "secret.yaml", "description: Not granted.\nrun: secret.sh\n");
}

} // namespace

// ── the allowlist ────────────────────────────────────────────────────────────

int test_allowlist_is_what_decides() {
    build_library();
    const fs::path ws = fs::temp_directory_path() / "funes_test_scriptlib_ws";
    fs::remove_all(ws);

    ToolRegistry reg;
    register_script_tools(reg, ws.string(), lib_dir().string());

    // An agent with no grants: installed and runnable are not the same thing.
    ToolContext none{"funes", "s1"};
    auto denied = reg.call("run_script", {{"name", "greet"}, {"arguments", {{"who", "x"}}}}, none);
    CHECK(denied.error);
    CHECK(denied.text.find("no scripts granted") != std::string::npos);
    CHECK(denied.text.find("args:") == std::string::npos);   // nothing ran

    auto listed = reg.call("list_scripts", json::object(), none);
    CHECK(listed.text.find("greet") == std::string::npos);

    // An agent granted one script cannot reach the one next to it.
    ToolContext one{"operator", "s1", "", "", 1, funes::Permissions::unrestricted(), {"greet"}};
    auto other = reg.call("run_script", {{"name", "secret"}}, one);
    CHECK(other.error);
    CHECK(other.text.find("not available to this agent") != std::string::npos);
    CHECK(other.text.find("ran-the-ungranted-script") == std::string::npos);

    // …and cannot reach it by dressing the name up as a path either. The
    // name is sanitized before it is ever joined onto the library directory,
    // so this is a refusal and not a traversal.
    for (const char* attempt : {"../scriptlib/secret", "secret.yaml", "/etc/passwd", "SECRET"}) {
        auto sneaky = reg.call("run_script", {{"name", attempt}}, one);
        CHECK(sneaky.error);
        CHECK(sneaky.text.find("ran-the-ungranted-script") == std::string::npos);
    }

    // Granted and installed: it runs, in the caller's own workspace.
    auto ok = reg.call("run_script", {{"name", "greet"}, {"arguments", {{"who", "world"}}}}, one);
    CHECK(!ok.error);
    CHECK(ok.text.find("exit_code: 0") != std::string::npos);
    CHECK(ok.text.find("args:--who=world") != std::string::npos);
    CHECK(ok.text.find("cwd:") != std::string::npos);
    CHECK(ok.text.find((ws / "1").string()) != std::string::npos);

    // list_scripts shows the grant, not the library.
    auto shown = reg.call("list_scripts", json::object(), one);
    CHECK(!shown.error);
    CHECK(shown.text.find("greet") != std::string::npos);
    CHECK(shown.text.find("who (string, required)") != std::string::npos);
    CHECK(shown.text.find("secret") == std::string::npos);

    fs::remove_all(ws);
    return 0;
}

// A grant naming a script the library doesn't have must say so — an agent
// allowed to run something absent is an admin's mistake, and the model
// otherwise reports it as its own failure and goes looking for a shell.
int test_granted_but_not_installed() {
    build_library();
    const fs::path ws = fs::temp_directory_path() / "funes_test_scriptlib_ws2";
    fs::remove_all(ws);

    ToolRegistry reg;
    register_script_tools(reg, ws.string(), lib_dir().string());
    ToolContext ctx{"operator", "s1", "", "", 1, funes::Permissions::unrestricted(),
                    {"nowhere", "ghost"}};

    auto missing = reg.call("run_script", {{"name", "nowhere"}}, ctx);
    CHECK(missing.error);
    CHECK(missing.text.find("No script named 'nowhere' is installed") != std::string::npos);
    CHECK(missing.text.find("administrator") != std::string::npos);

    // A manifest whose `run:` file is absent fails the same way, at load.
    auto ghost = reg.call("run_script", {{"name", "ghost"}}, ctx);
    CHECK(ghost.error);
    CHECK(ghost.text.find("not installed") != std::string::npos);

    // Neither one is listable either, and the list says which side is wrong.
    auto listed = reg.call("list_scripts", json::object(), ctx);
    CHECK(listed.error);
    CHECK(listed.text.find("disagree") != std::string::npos);

    fs::remove_all(ws);
    return 0;
}

// ── arguments ────────────────────────────────────────────────────────────────

int test_arguments() {
    build_library();
    funes::ScriptSpec spec;
    CHECK(funes::load_script(lib_dir().string(), "greet", spec).empty());
    CHECK(spec.name == "greet");
    CHECK(spec.params.size() == 4);
    CHECK(spec.params[0].required);
    CHECK(spec.timeout_seconds == 10);

    std::vector<std::string> argv;

    // One token per parameter, in manifest order, after the script path.
    CHECK(funes::build_argv(spec, {{"who", "ana"}, {"times", 3}, {"loud", true}}, argv).empty());
    CHECK(argv.size() == 4);
    CHECK(argv[0] == spec.file);
    CHECK(argv[1] == "--who=ana");
    CHECK(argv[2] == "--times=3");
    CHECK(argv[3] == "--loud");            // a flag is presence, not "--loud=true"

    // A false boolean is absent, not "--loud=false".
    argv.clear();
    CHECK(funes::build_argv(spec, {{"who", "ana"}, {"loud", false}}, argv).empty());
    CHECK(argv.size() == 2);

    // Shell metacharacters are data. There is no shell, so this is one
    // argument with punctuation in it — the property that makes run_script a
    // narrower grant than execute_shell rather than a longer spelling of it.
    argv.clear();
    CHECK(funes::build_argv(spec, {{"who", "; rm -rf /"}}, argv).empty());
    CHECK(argv[1] == "--who=; rm -rf /");

    // A value that starts with a dash cannot become a flag: one token, glued.
    argv.clear();
    CHECK(funes::build_argv(spec, {{"who", "--force"}}, argv).empty());
    CHECK(argv[1] == "--who=--force");

    // Refusals, each naming what is wrong.
    argv.clear();
    CHECK(!funes::build_argv(spec, json::object(), argv).empty());                 // required missing
    CHECK(argv.empty());                                                            // nothing built
    CHECK(!funes::build_argv(spec, {{"who", "a"}, {"nope", 1}}, argv).empty());     // undeclared
    CHECK(!funes::build_argv(spec, {{"who", "a"}, {"times", "many"}}, argv).empty());// not a number
    CHECK(!funes::build_argv(spec, {{"who", "a"}, {"mode", "sideways"}}, argv).empty()); // not a choice
    CHECK(funes::build_argv(spec, {{"who", "a"}, {"mode", "fast"}}, argv).empty());
    CHECK(!funes::build_argv(spec, {{"who", std::string("a\nb")}}, argv).empty());  // control character
    CHECK(!funes::build_argv(spec, {{"who", json::array({"a"})}}, argv).empty());   // wrong shape

    // A number sent as a digit string is accepted — small models do this —
    // while prose is not.
    argv.clear();
    CHECK(funes::build_argv(spec, {{"who", "a"}, {"times", "7"}}, argv).empty());
    CHECK(argv[2] == "--times=7");
    return 0;
}

// ── manifests ────────────────────────────────────────────────────────────────

int test_manifest_rules() {
    build_library();
    const std::string dir = lib_dir().string();

    // `run:` may not leave the library — the directory is the boundary.
    write_file(lib_dir() / "escape.yaml", "description: x\nrun: ../../bin/funes\n");
    funes::ScriptSpec spec;
    CHECK(!funes::load_script(dir, "escape", spec).empty());

    // A name that is not [a-z0-9_-] is refused rather than sanitized into a
    // different script's name.
    CHECK(!funes::load_script(dir, "../secret", spec).empty());
    CHECK(!funes::load_script(dir, "Greet", spec).empty());
    CHECK(funes::safe_script_name("../secret") == "secret");   // …which is why it is compared

    // The timeout is clamped to the same ceiling execute_shell uses.
    write_file(lib_dir() / "forever.yaml",
               "description: x\nrun: greet.sh\ntimeout_seconds: 99999\n");
    CHECK(funes::load_script(dir, "forever", spec).empty());
    CHECK(spec.timeout_seconds == 120);

    // A bad parameter type is a load error, not a surprise at call time.
    write_file(lib_dir() / "badparam.yaml",
               "description: x\nrun: greet.sh\nparams:\n  - name: p\n    type: date\n");
    CHECK(!funes::load_script(dir, "badparam", spec).empty());

    // list_script_names sees the library; it is not what decides anything.
    const auto names = funes::list_script_names(dir);
    CHECK(std::find(names.begin(), names.end(), "greet") != names.end());
    CHECK(std::find(names.begin(), names.end(), "secret") != names.end());

    // load_scripts skips a grant the library doesn't have rather than failing
    // the whole list: an agent YAML and the library are edited separately.
    const auto loaded = funes::load_scripts(dir, {"greet", "nowhere"});
    CHECK(loaded.size() == 1);
    CHECK(loaded[0].name == "greet");
    return 0;
}

// ── runtime behaviour ────────────────────────────────────────────────────────

int test_execution_details() {
    build_library();
    const fs::path ws = fs::temp_directory_path() / "funes_test_scriptlib_ws3";
    fs::remove_all(ws);

    ToolRegistry reg;
    register_script_tools(reg, ws.string(), lib_dir().string());
    ToolContext ctx{"operator", "s1", "", "", 7, funes::Permissions::unrestricted(),
                    {"greet", "slow"}};

    // A manifest's env reaches the process; the model never named the value.
    setenv("FUNES_TEST_SCRIPT_SECRET", "from-the-manifest", 1);
    auto r = reg.call("run_script", {{"name", "greet"}, {"arguments", {{"who", "x"}}}}, ctx);
    CHECK(!r.error);
    CHECK(r.text.find("env:from-the-manifest") != std::string::npos);
    // …and the rest of the environment survives the override.
    CHECK(getenv("PATH") != nullptr);
    unsetenv("FUNES_TEST_SCRIPT_SECRET");

    // Each account runs in its own workspace.
    CHECK(r.text.find((ws / "7").string()) != std::string::npos);

    // The script's own timeout is enforced, and the process is killed.
    auto slow = reg.call("run_script", {{"name", "slow"}}, ctx);
    CHECK(slow.error);
    CHECK(slow.text.find("TIMED OUT") != std::string::npos);

    // Shell execution being off changes nothing: this grant is not that one.
    unsetenv("FUNES_ALLOW_SHELL");
    auto still = reg.call("run_script", {{"name", "greet"}, {"arguments", {{"who", "y"}}}}, ctx);
    CHECK(!still.error);
    CHECK(still.text.find("args:--who=y") != std::string::npos);

    fs::remove_all(ws);
    return 0;
}

// ── the YAML field ───────────────────────────────────────────────────────────

int test_agent_yaml() {
    AgentConfig granted = AgentConfig::from_string(
        "name: runner\ntools: [run_script]\nscripts: [backup_db, workspace_report]\n");
    CHECK(granted.scripts.size() == 2);
    CHECK(granted.scripts[0] == "backup_db");

    // Absent means none — the opposite of `tools:`, where absent means all.
    AgentConfig bare = AgentConfig::from_string("name: plain\ntools: [run_script]\n");
    CHECK(bare.scripts.empty());

    // The shipped agent that trades shell for scripts.
    AgentConfig op = AgentConfig::from_file("agents/operator.yaml");
    CHECK(!op.scripts.empty());
    for (const auto& name : op.scripts) {
        funes::ScriptSpec spec;
        const std::string err = funes::load_script("scriptlib", name, spec);
        CHECK(err.empty());          // every grant in the repo names a real script
        CHECK(!spec.description.empty());
    }
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_allowlist_is_what_decides();
    rc |= test_granted_but_not_installed();
    rc |= test_arguments();
    rc |= test_manifest_rules();
    rc |= test_execution_details();
    rc |= test_agent_yaml();
    if (rc == 0) std::cout << "test_script_library: all tests passed\n";
    return rc;
}
