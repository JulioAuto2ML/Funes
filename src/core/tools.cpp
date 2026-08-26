// =============================================================================
// src/core/tools.cpp — tool registry implementation
// =============================================================================

#include <iostream>
#include "tools.h"
#include <stdexcept>

void ToolRegistry::add(NativeTool tool) {
    if (tool.name.empty())
        throw std::runtime_error("ToolRegistry::add: tool name must be non-empty");
    tools_[tool.name] = std::move(tool);
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const auto& [name, _] : tools_)
        out.push_back(name);
    return out;
}

json ToolRegistry::openai_schema(const std::vector<std::string>& allowlist) const {
    json arr = json::array();
    for (const auto& [name, tool] : tools_) {
        if (!allowlist.empty()) {
            bool allowed = false;
            for (const auto& a : allowlist)
                if (a == name) { allowed = true; break; }
            if (!allowed) continue;
        }
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name",        tool.name},
                {"description", tool.description},
                {"parameters",  tool.parameters.is_null()
                                    ? json{{"type","object"},{"properties",json::object()}}
                                    : tool.parameters}
            }}
        });
    }
    return arr;
}

ToolResult ToolRegistry::call(const std::string& name, const json& args,
                              const ToolContext& ctx) const {
    auto it = tools_.find(name);
    if (it == tools_.end())
        return {"Unknown tool: " + name, /*error=*/true};

    try {
        return it->second.handler(args.is_object() ? args : json::object(), ctx);
    } catch (const std::exception& e) {
        // A throwing tool is the one failure the model sees and the operator
        // does not. harvest_candidates failed this way for two days
        // (2026-08-25/26): the model was told "type mismatch" and paraphrased
        // it into an answer, while the journal recorded nothing but the budget
        // refusals that followed. The arguments go in too — a type error is
        // usually about what came in.
        std::cerr << "[tools] " << name << " THREW: " << e.what()
                  << " — args=" << (args.is_object() ? args.dump().substr(0, 300)
                                                     : std::string("(not an object)"))
                  << "\n";
        return {std::string("Tool '") + name + "' failed: " + e.what(), /*error=*/true};
    }
}
