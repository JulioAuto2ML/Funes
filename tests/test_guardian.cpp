// =============================================================================
// tests/test_guardian.cpp — the server's side of the guardian (self-healing H1)
// =============================================================================
// The guardian itself is tested in scripts/tests/test_funes_guardian.py. Here:
// "deshacer N" is recognised exactly as narrowly as "aprobar N", and
// create_agent — Funes rewriting its own agents — snapshots before it writes,
// reports the change after, and does not write at all when the guardian
// cannot take the snapshot. The guardian is a fake Python script that logs
// its arguments and answers from a control file.

#include "guardian.h"
#include "tools.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const fs::path kDir = fs::temp_directory_path() / "funes_test_guardian";

std::string read(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

void fake_guardian() {
    fs::remove_all(kDir);
    fs::create_directories(kDir / "agents");
    std::ofstream(kDir / "guardian.py") << R"py(
import json, sys, pathlib
here = pathlib.Path(__file__).parent
with open(here / "calls.log", "a") as f:
    f.write(" ".join(sys.argv[3:]) + "\n")
cmd = sys.argv[3]
if (here / "fail").exists():
    print(json.dumps({"ok": False, "error": "snapshot failed: disk full"})); sys.exit(1)
if cmd == "begin": print(json.dumps({"id": 7}))
elif cmd == "applied": print(json.dumps({"ok": True}))
elif cmd == "undo": print(json.dumps({"ok": True, "message": "↩️ Cambio #" + sys.argv[4] + " deshecho"}))
)py";
    funes::guardian::configure((kDir / "guardian.py").string(), kDir.string());
}

void add_recall(ToolRegistry& reg) {
    reg.add({"recall", "recall", {{"type", "object"}, {"properties", json::object()}},
             [](const json&, const ToolContext&) { return ToolResult{"ok"}; }});
}

json agent_args(const std::string& name) {
    return {{"name", name}, {"description", "d"}, {"system_prompt", "You help."},
            {"tools", json::array({"recall"})}, {"overwrite", true}};
}

} // namespace

int test_parse_undo() {
    using funes::guardian::parse_undo;
    CHECK(parse_undo("deshacer 3") == 3);
    CHECK(parse_undo("  Deshacer #12. ") == 12);
    CHECK(parse_undo("UNDO 4!") == 4);
    CHECK(!parse_undo("deshacer el cambio 3"));
    CHECK(!parse_undo("deshacer"));
    CHECK(!parse_undo("deshacer 3 por favor"));
    CHECK(!parse_undo("aprobar 3"));
    CHECK(!parse_undo("deshacer3"));
    return 0;
}

int test_create_agent_goes_through_the_guardian() {
    fake_guardian();
    ToolRegistry reg;
    add_recall(reg);
    register_agent_builder(reg, (kDir / "agents").string(), [] {});
    auto r = reg.call("create_agent", agent_args("helper"), ToolContext{"agent-doctor", "s"});
    CHECK(!r.error);
    CHECK(fs::exists(kDir / "agents" / "helper.yaml"));
    CHECK(r.text.find("deshacer 7") != std::string::npos);
    const std::string calls = read(kDir / "calls.log");
    const size_t b = calls.find("begin");
    const size_t a = calls.find("applied 7");
    CHECK(b != std::string::npos && a != std::string::npos && b < a);
    CHECK(calls.find("helper.yaml") != std::string::npos);
    CHECK(calls.find("--restart funes") != std::string::npos);
    return 0;
}

// No snapshot, no change.
int test_no_snapshot_no_write() {
    fake_guardian();
    std::ofstream(kDir / "fail") << "1";
    ToolRegistry reg;
    add_recall(reg);
    register_agent_builder(reg, (kDir / "agents").string(), [] {});
    auto r = reg.call("create_agent", agent_args("helper"), ToolContext{"agent-doctor", "s"});
    CHECK(r.error);
    CHECK(r.text.find("disk full") != std::string::npos);
    CHECK(!fs::exists(kDir / "agents" / "helper.yaml"));
    return 0;
}

int test_undo_relays_the_guardians_answer() {
    fake_guardian();
    auto r = funes::guardian::undo(5, "julio");
    CHECK(r.ok);
    CHECK(r.message.find("#5") != std::string::npos);
    return 0;
}

// Without a guardian (a development checkout), create_agent works as before.
int test_off_means_unchanged() {
    fake_guardian();
    funes::guardian::configure("", "");
    CHECK(!funes::guardian::enabled());
    ToolRegistry reg;
    add_recall(reg);
    register_agent_builder(reg, (kDir / "agents").string(), [] {});
    auto r = reg.call("create_agent", agent_args("plain"), ToolContext{"agent-doctor", "s"});
    CHECK(!r.error);
    CHECK(fs::exists(kDir / "agents" / "plain.yaml"));
    CHECK(!fs::exists(kDir / "calls.log"));
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_parse_undo();
    rc |= test_create_agent_goes_through_the_guardian();
    rc |= test_no_snapshot_no_write();
    rc |= test_undo_relays_the_guardians_answer();
    rc |= test_off_means_unchanged();
    if (rc == 0) std::cout << "test_guardian: all passed\n";
    return rc;
}
