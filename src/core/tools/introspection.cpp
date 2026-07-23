// =============================================================================
// src/core/tools/introspection.cpp — list_tools native tool
// =============================================================================
// Lets the agent-builder (and anyone else) see what tools actually exist
// before drafting a new agent's tool allowlist, instead of guessing names.

#include "../tools.h"
#include <sstream>

namespace {

ToolResult list_tools_handler(const ToolRegistry& reg, const json&, const ToolContext&) {
    json schema = reg.openai_schema();
    if (schema.empty())
        return {"No tools are registered."};

    std::ostringstream oss;
    for (const auto& t : schema) {
        const auto& fn = t["function"];
        oss << "- " << fn.value("name", "?") << ": " << fn.value("description", "") << "\n";
    }
    return {oss.str()};
}

} // namespace

void register_introspection_tools(ToolRegistry& reg) {
    reg.add({
        "list_tools",
        "List every tool currently available in Funes, with its name and description. "
        "Use this before choosing tools for a new agent, or before scaffolding a new "
        "tool, so you don't duplicate or reference something that doesn't exist.",
        {{"type", "object"}, {"properties", json::object()}},
        [&reg](const json& args, const ToolContext& ctx) {
            return list_tools_handler(reg, args, ctx);
        }
    });
}
