// =============================================================================
// tests/test_meta_tools.cpp — list_tools, create_tool, create_agent
// =============================================================================

#include "tools.h"
#include "agent_config.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {
ToolResult ok_handler(const json&, const ToolContext&) { return {"ok"}; }
}

int test_list_tools() {
    ToolRegistry reg;
    reg.add({"foo", "does foo", {{"type","object"},{"properties",json::object()}}, ok_handler});
    register_introspection_tools(reg);

    ToolContext ctx{"a", "s"};
    auto r = reg.call("list_tools", json::object(), ctx);
    CHECK(!r.error);
    CHECK(r.text.find("foo") != std::string::npos);
    CHECK(r.text.find("list_tools") != std::string::npos);  // self-listed
    return 0;
}

int test_create_tool() {
    fs::path dir = fs::temp_directory_path() / "funes_test_generated";
    fs::remove_all(dir);
    ToolRegistry reg;
    register_tool_builder(reg, dir.string());
    ToolContext ctx{"a", "s"};

    auto bad_name = reg.call("create_tool",
        {{"name", "Bad Name!"}, {"description", "x"}, {"url_template", "https://example.com"}}, ctx);
    CHECK(bad_name.error);

    auto bad_scheme = reg.call("create_tool",
        {{"name", "get_thing"}, {"description", "x"}, {"url_template", "ftp://example.com"}}, ctx);
    CHECK(bad_scheme.error);

    auto ok = reg.call("create_tool", {
        {"name", "get_weather"},
        {"description", "Gets the weather"},
        {"method", "GET"},
        {"url_template", "https://api.example.com/v1/weather?city={city}"},
        {"parameters", {
            {"type", "object"},
            {"properties", {{"city", {{"type", "string"}}}}},
            {"required", json::array({"city"})}
        }},
        {"headers", {{"Authorization", "Bearer ${FUNES_TEST_TOKEN}"}}}
    }, ctx);
    CHECK(!ok.error);

    fs::path generated = dir / "get_weather.cpp";
    CHECK(fs::exists(generated));
    std::ifstream f(generated);
    std::ostringstream contents;
    contents << f.rdbuf();
    const std::string src = contents.str();
    CHECK(src.find("register_get_weather") != std::string::npos);
    CHECK(src.find("make_http_template_tool") != std::string::npos);
    CHECK(src.find("${FUNES_TEST_TOKEN}") != std::string::npos);
    // Raw string delimiters longer than 16 chars fail to compile (a real bug
    // caught by manual testing: "FUNES_SCHEMA_" + an uppercased tool name
    // blew past that limit) — pin the delimiter to something short and fixed.
    CHECK(src.find("R\"SCHEMA(") != std::string::npos);

    auto dup = reg.call("create_tool",
        {{"name", "get_weather"}, {"description", "x"}, {"url_template", "https://example.com"}}, ctx);
    CHECK(dup.error);

    auto redo = reg.call("create_tool", {
        {"name", "get_weather"}, {"description", "x2"},
        {"url_template", "https://example.com"}, {"overwrite", true}
    }, ctx);
    CHECK(!redo.error);

    fs::remove_all(dir);
    return 0;
}

int test_create_agent() {
    fs::path dir = fs::temp_directory_path() / "funes_test_agents";
    fs::remove_all(dir);
    fs::create_directories(dir);

    ToolRegistry reg;
    reg.add({"recall", "recall stuff", {{"type","object"},{"properties",json::object()}}, ok_handler});
    int reload_calls = 0;
    register_agent_builder(reg, dir.string(), [&reload_calls] { ++reload_calls; });

    ToolContext ctx{"a", "s"};

    auto bad = reg.call("create_agent", {
        {"name", "my-agent"}, {"description", "d"}, {"system_prompt", "p"},
        {"tools", json::array({"not_a_real_tool"})}
    }, ctx);
    CHECK(bad.error);
    CHECK(reload_calls == 0);

    auto ok = reg.call("create_agent", {
        {"name", "my-agent"}, {"description", "A test agent"},
        {"system_prompt", "Be helpful.\n"}, {"tools", json::array({"recall"})}
    }, ctx);
    CHECK(!ok.error);
    CHECK(reload_calls == 1);

    fs::path path = dir / "my-agent.yaml";
    CHECK(fs::exists(path));

    AgentConfig cfg = AgentConfig::from_file(path.string());
    CHECK(cfg.name == "my-agent");
    CHECK(cfg.description == "A test agent");
    CHECK(cfg.tools.size() == 1 && cfg.tools[0] == "recall");
    CHECK(cfg.system_prompt.find("Be helpful.") != std::string::npos);

    auto dup = reg.call("create_agent", {
        {"name", "my-agent"}, {"description", "d"}, {"system_prompt", "p"}, {"tools", json::array()}
    }, ctx);
    CHECK(dup.error);
    CHECK(reload_calls == 1);

    fs::remove_all(dir);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_list_tools();
    rc |= test_create_tool();
    rc |= test_create_agent();
    if (rc == 0) std::cout << "test_meta_tools: all tests passed\n";
    return rc;
}
