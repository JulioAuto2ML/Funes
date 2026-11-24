// =============================================================================
// tests/test_practice_tools.cpp — the coach's tools (tools/practice_tools.cpp)
// =============================================================================
// Through the registry, against the shipped library and a temporary
// workspace: the plan is checked and written where the Practice view reads it,
// an invented exercise is refused by name, and one account never sees
// another's range, sessions or plan.

#include "practice_store.h"
#include "tools.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace funes::practice;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: test_practice_tools <practice dir>\n"; return 2; }
    const std::string library = argv[1];
    const fs::path ws = fs::temp_directory_path() / ("funes_practice_tools_" + std::to_string(::getpid()));
    fs::remove_all(ws);
    fs::create_directories(ws);

    ToolRegistry reg;
    register_practice_tools(reg, ws.string(), library, [](int64_t) { return std::string("es"); });
    ToolContext julio("vocal-coach", "s1", "", "", 1);
    ToolContext other("vocal-coach", "s2", "", "", 2);

    // No range yet: the list says so, and a plan is refused.
    auto r = reg.call("practice_exercises", json::object(), julio);
    CHECK(!r.error && r.text.find("No range yet") != std::string::npos);
    r = reg.call("practice_plan", {{"items", {{{"id", "hum_scale"}}}}}, julio);
    CHECK(r.error);

    // With a range: titles in the caller's language, and a plan is written.
    Profile p;
    CHECK(profile_from_json({{"voice", "baritone"}, {"full", {"G2", "G4"}}, {"comfortable", {"A2", "D4"}}}, p).empty());
    CHECK(write_json(user_dir(ws.string(), 1) / "profile.json", profile_to_json(p)));
    r = reg.call("practice_exercises", json::object(), julio);
    CHECK(r.text.find("Escala de 5 notas") != std::string::npos);
    CHECK(r.text.find("reps fit") != std::string::npos);

    r = reg.call("practice_plan", {{"items", {{{"id", "lip_trill_scale"}, {"reps", 6}}, {{"id", "breath_hiss"}}}},
                                   {"note", "Calentar suave."}}, julio);
    CHECK(!r.error);
    auto plan = load_plan(user_dir(ws.string(), 1));
    CHECK(plan && (*plan)["items"].size() == 2 && (*plan)["by"] == "vocal-coach");
    CHECK((*plan)["items"][0]["reps"] == 6);

    r = reg.call("practice_plan", {{"items", {{{"id", "yodel_drill"}}}}}, julio);
    CHECK(r.error && r.text.find("yodel_drill") != std::string::npos);

    // History: what was sung, and the window.
    CHECK(append_session(user_dir(ws.string(), 1),
        {{"at", now_iso()}, {"exercise", "hum_scale"}, {"scores", {{"pitch", 77}}}}));
    r = reg.call("practice_history", {{"days", 7}}, julio);
    CHECK(r.text.find("hum_scale") != std::string::npos && r.text.find("77%") != std::string::npos);

    // Another account sees none of it.
    r = reg.call("practice_history", json::object(), other);
    CHECK(r.text.find("hum_scale") == std::string::npos);
    CHECK(r.text.find("No range yet") != std::string::npos);
    CHECK(!load_plan(user_dir(ws.string(), 2)));

    fs::remove_all(ws);
    std::cout << "test_practice_tools: all passed\n";
    return 0;
}
