// =============================================================================
// tests/test_tools.cpp — ToolRegistry + memory tools unit tests
// =============================================================================

#include "tools.h"
#include "memory.h"
#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_registry() {
    ToolRegistry reg;
    reg.add({
        "echo", "Echoes the input",
        {{"type","object"},
         {"properties", {{"text", {{"type","string"}}}}},
         {"required", json::array({"text"})}},
        [](const json& args, const ToolContext&) -> ToolResult {
            return {args.value("text", "")};
        }
    });
    reg.add({
        "boom", "Always throws", json(nullptr),
        [](const json&, const ToolContext&) -> ToolResult {
            throw std::runtime_error("kaboom");
        }
    });

    CHECK(reg.has("echo"));
    CHECK(!reg.has("nope"));

    // Schema: full and allowlist-filtered.
    json schema = reg.openai_schema();
    CHECK(schema.size() == 2);
    CHECK(schema[1]["function"]["name"] == "echo" || schema[0]["function"]["name"] == "echo");

    json filtered = reg.openai_schema({"echo"});
    CHECK(filtered.size() == 1);
    CHECK(filtered[0]["function"]["name"] == "echo");
    // Null parameters become an empty object schema.
    json boom_only = reg.openai_schema({"boom"});
    CHECK(boom_only[0]["function"]["parameters"]["type"] == "object");

    // Dispatch.
    ToolContext ctx{"funes", "s1"};
    auto r = reg.call("echo", {{"text", "hola"}}, ctx);
    CHECK(!r.error && r.text == "hola");

    // Exceptions become error results, never propagate.
    auto rb = reg.call("boom", json::object(), ctx);
    CHECK(rb.error);
    CHECK(rb.text.find("kaboom") != std::string::npos);

    // Unknown tool.
    auto ru = reg.call("nope", json::object(), ctx);
    CHECK(ru.error);
    return 0;
}

int test_memory_tools() {
    fs::path db = fs::temp_directory_path() / "funes_test_memtools.db";
    fs::remove(db);
    MemoryStore store(db.string(), nullptr);

    ToolRegistry reg;
    register_memory_tools(reg, store);
    CHECK(reg.has("remember") && reg.has("recall"));

    ToolContext ctx{"funes", "s1"};

    // remember stores under the calling agent, source "tool".
    auto r1 = reg.call("remember", {{"text", "User prefers espresso over filter coffee"}}, ctx);
    CHECK(!r1.error);
    CHECK(store.count("funes") == 1);
    CHECK(store.list("funes")[0].source == "tool");

    // recall finds it.
    auto r2 = reg.call("recall", {{"query", "espresso"}}, ctx);
    CHECK(!r2.error);
    CHECK(r2.text.find("espresso") != std::string::npos);

    // recall from a different agent's context sees nothing.
    ToolContext other{"researcher", "s1"};
    auto r3 = reg.call("recall", {{"query", "espresso"}}, other);
    CHECK(r3.text.find("No memories found") != std::string::npos);

    // Validation errors.
    CHECK(reg.call("remember", json::object(), ctx).error);
    CHECK(reg.call("recall", json::object(), ctx).error);
    CHECK(reg.call("remember", {{"text", std::string(3000, 'x')}}, ctx).error);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_registry();
    rc |= test_memory_tools();
    if (rc == 0) std::cout << "test_tools: all tests passed\n";
    return rc;
}
