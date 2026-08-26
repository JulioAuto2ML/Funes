// =============================================================================
// src/core/context_compressor.h — fold old turns into a rolling summary
// =============================================================================
//
// Shared by two call sites:
//   - FunesAgent::run(): an automatic safety net that fires when the estimated
//     prompt size is about to crowd out the context window.
//   - the `compress_context` native tool: an explicit, on-demand version of
//     the same operation (see src/core/tools/context_tools.cpp).
//
// The algorithm is intentionally simple: fold the oldest half of the loaded
// turn window into the session's single running summary (via one LLM call),
// then prune those turns from storage. The summary replaces itself each time
// rather than accumulating, so it stays roughly constant size no matter how
// long the conversation runs.
// =============================================================================

#pragma once
#include "llm_client.h"
#include <string>
#include <vector>

class MemoryStore;

// chars/4 is a rough, model-agnostic proxy for token count — good enough to
// decide "are we getting close", not meant to match any real tokenizer.
inline int estimate_tokens(const std::string& text) {
    return static_cast<int>(text.size() / 4);
}

// Flat per-image guess (vision APIs typically spend a few hundred to a
// couple thousand tokens per image depending on resolution/provider) — a
// base64 byte count isn't a text-token proxy the way chars/4 is, so this
// isn't trying to be precise, just non-zero.
constexpr int kEstimatedTokensPerImage = 800;

inline int estimate_tokens(const std::vector<ChatMessage>& messages) {
    size_t chars = 0;
    size_t images = 0;
    for (const auto& m : messages) {
        chars += m.content.size();
        if (!m.tool_calls.is_null()) chars += m.tool_calls.dump().size();
        images += m.images.size();
    }
    return static_cast<int>(chars / 4) + static_cast<int>(images) * kEstimatedTokensPerImage;
}

struct CompressOutcome {
    bool        compressed = false;
    int         turns_folded = 0;
    std::string summary_preview;  // first ~200 chars of the new summary
};

// Folds the oldest half of `recent` (at least `min_keep` messages are always
// left untouched) into the session's running summary, one LLM call. Updates
// `recent` and `summary` in place so the caller can use the shrunk window
// immediately, and persists both the new summary and the pruned turn table.
//
// No-op (compressed=false) if `recent` already has `min_keep` or fewer turns.
CompressOutcome compress_oldest_half(MemoryStore& memory, LLMClient& llm,
                                     int64_t user_id,
                                     const std::string& session, const std::string& agent,
                                     std::vector<ChatMessage>& recent, std::string& summary,
                                     int min_keep = 4);
