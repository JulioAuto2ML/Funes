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

constexpr size_t kMaxRememberChars = 500;

ToolResult remember_handler(MemoryStore& store, const json& args, const ToolContext& ctx) {
    if (!args.contains("text") || !args["text"].is_string()
        || args["text"].get<std::string>().empty())
        return {"Missing or invalid 'text' argument", /*error=*/true};

    // One sentence, not a document (docs/memory-plan.md, phase 2). Counted in
    // characters so Spanish isn't charged twice for its accents. A paragraph
    // stored here is recalled whole into every prompt it matches; the text
    // belongs in a file and the memory in a sentence that points at it.
    const std::string text = args["text"].get<std::string>();
    size_t chars = 0;
    for (unsigned char c : text) chars += (c & 0xC0) != 0x80;
    if (chars > kMaxRememberChars)
        return {"Memory text too long (" + std::to_string(chars) + " characters, max "
                + std::to_string(kMaxRememberChars) + "). Store one self-contained sentence; "
                "if the detail matters, write it to a file with write_file and remember "
                "where it is.", true};

    // After a page, a search or an email, what the model writes down may be
    // that source's claim rather than the person's (5.0.1 review, #8). Marked
    // tainted: weighted like a conversation log, not like a taught fact, and
    // shown back as an attribution — see memory_provenance.h.
    funes::Provenance prov;
    if (ctx.provenance && ctx.provenance->tainted()) {
        prov.origin  = ctx.provenance->describe();
        prov.tainted = true;
    } else {
        prov.origin = "session:" + ctx.session;
    }
    int64_t id = store.remember(ctx.user_id, ctx.memory_scope, text, "tool", prov);
    if (prov.tainted)
        return {"Remembered as a claim from " + prov.origin + " (memory #"
                + std::to_string(id) + ")."};
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

    auto results = store.recall(ctx.user_id, ctx.memory_scope, query, k);
    if (results.empty())
        return {"No memories found for: " + query};

    std::ostringstream oss;
    for (size_t i = 0; i < results.size(); ++i) {
        if (i > 0) oss << "\n";
        oss << "- [" << results[i].created_at << "] " << results[i].presented();
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
