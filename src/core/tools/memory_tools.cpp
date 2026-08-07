// =============================================================================
// src/core/tools/memory_tools.cpp — remember / recall native tools
// =============================================================================
// The explicit face of Funes' memory: the agent decides what is worth keeping
// and can search its own past deliberately. (Automatic memory injection and
// turn storage happen in the agent loop, independent of these tools.)

#include "../tools.h"
#include "../memory.h"
#include <sstream>

namespace {

ToolResult remember_handler(MemoryStore& store, const json& args, const ToolContext& ctx) {
    if (!args.contains("text") || !args["text"].is_string()
        || args["text"].get<std::string>().empty())
        return {"Missing or invalid 'text' argument", /*error=*/true};

    const std::string text = args["text"].get<std::string>();
    if (text.size() > 2000)
        return {"Memory text too long (max 2000 chars) — store a summary instead", true};

    int64_t id = store.remember(ctx.memory_scope, text, "tool");
    return {"Remembered (memory #" + std::to_string(id) + ")."};
}

ToolResult recall_handler(MemoryStore& store, const json& args, const ToolContext& ctx) {
    if (!args.contains("query") || !args["query"].is_string()
        || args["query"].get<std::string>().empty())
        return {"Missing or invalid 'query' argument", /*error=*/true};

    const std::string query = args["query"].get<std::string>();
    int k = 5;
    if (args.contains("k") && args["k"].is_number_integer()) {
        k = args["k"].get<int>();
        if (k < 1)  k = 1;
        if (k > 20) k = 20;
    }

    auto results = store.recall(ctx.memory_scope, query, k);
    if (results.empty())
        return {"No memories found for: " + query};

    std::ostringstream oss;
    for (size_t i = 0; i < results.size(); ++i) {
        if (i > 0) oss << "\n";
        oss << "- [" << results[i].created_at << "] " << results[i].text;
    }
    return {oss.str()};
}

} // namespace

void register_memory_tools(ToolRegistry& reg, MemoryStore& store) {
    reg.add({
        "remember",
        "Store a fact in persistent memory so it survives across conversations. "
        "Use short, self-contained sentences (who/what, no pronouns).",
        {
            {"type", "object"},
            {"properties", {
                {"text", {{"type", "string"},
                          {"description", "The fact to remember, as one self-contained sentence"}}}
            }},
            {"required", json::array({"text"})}
        },
        [&store](const json& args, const ToolContext& ctx) {
            return remember_handler(store, args, ctx);
        }
    });

    reg.add({
        "recall",
        "Search persistent memory for facts related to a query. Returns the "
        "most relevant memories with their dates.",
        {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}, {"description", "What to search for"}}},
                {"k",     {{"type", "integer"},
                           {"description", "How many memories to return (default 5, max 20)"}}}
            }},
            {"required", json::array({"query"})}
        },
        [&store](const json& args, const ToolContext& ctx) {
            return recall_handler(store, args, ctx);
        }
    });
}
