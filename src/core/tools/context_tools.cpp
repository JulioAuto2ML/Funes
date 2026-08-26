// =============================================================================
// src/core/tools/context_tools.cpp — compress_context native tool
// =============================================================================
// The explicit, on-demand counterpart to the automatic compression safety net
// in FunesAgent::run() (see ../context_compressor.h). Lets the user or the
// model say "compress our conversation" and have it happen immediately,
// regardless of how close to the context limit the session currently is.

#include "../agent.h"        // AgentDefaults
#include "../context_compressor.h"
#include "../memory.h"
#include "../tools.h"

namespace {

constexpr int kManualMinKeep = 2;
constexpr int kMaxTurnsFetched = 2000;  // effectively "all of them"

ToolResult compress_context_handler(MemoryStore& store, const AgentDefaults& defaults,
                                    const json&, const ToolContext& ctx) {
    if (ctx.session.empty())
        return {"No active session to compress.", /*error=*/true};

    std::string summary = store.get_summary(ctx.user_id, ctx.session);
    std::vector<ChatMessage> recent = store.recent_turns(ctx.user_id, ctx.session, kMaxTurnsFetched);

    if (static_cast<int>(recent.size()) <= kManualMinKeep)
        return {"Nothing to compress yet — this conversation is still short."};

    LLMClient llm(defaults.llm_url, defaults.llm_api_key,
                 defaults.llm_model.empty() ? "default" : defaults.llm_model,
                 defaults.llm_provider);

    CompressOutcome result = compress_oldest_half(store, llm, ctx.user_id, ctx.session, ctx.agent,
                                                  recent, summary, kManualMinKeep);
    if (!result.compressed)
        return {"Compression didn't produce a shorter summary — leaving the "
                "conversation as-is.", /*error=*/true};

    return {"Compressed " + std::to_string(result.turns_folded) +
            " older turn(s) into a summary: " + result.summary_preview};
}

} // namespace

void register_context_tools(ToolRegistry& reg, MemoryStore& store, const AgentDefaults& defaults) {
    reg.add({
        "compress_context",
        "Fold the oldest part of this conversation into a running summary, freeing up "
        "context space. Use when a conversation has gotten long and you're worried about "
        "running out of context, or when the user explicitly asks to compress/summarize "
        "the conversation.",
        {{"type", "object"}, {"properties", json::object()}},
        [&store, &defaults](const json& args, const ToolContext& ctx) {
            return compress_context_handler(store, defaults, args, ctx);
        }
    });
}
