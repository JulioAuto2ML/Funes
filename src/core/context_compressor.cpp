// =============================================================================
// src/core/context_compressor.cpp — compress_oldest_half implementation
// =============================================================================

#include "context_compressor.h"
#include "memory.h"
#include <algorithm>
#include <iostream>
#include <sstream>

namespace {

std::string build_transcript(const std::string& existing_summary,
                             const std::vector<ChatMessage>& folded) {
    std::ostringstream oss;
    if (!existing_summary.empty())
        oss << "Summary of the conversation so far:\n" << existing_summary << "\n\n";
    oss << "Additional turns to fold into that summary:\n";
    for (const auto& m : folded)
        oss << (m.role == "user" ? "User: " : "Assistant: ") << m.content << "\n";
    return oss.str();
}

} // namespace

CompressOutcome compress_oldest_half(MemoryStore& memory, LLMClient& llm,
                                     const std::string& session, const std::string& agent,
                                     std::vector<ChatMessage>& recent, std::string& summary,
                                     int min_keep) {
    CompressOutcome out;
    if (min_keep < 2) min_keep = 2;
    if (static_cast<int>(recent.size()) <= min_keep) return out;

    const size_t keep      = std::max<size_t>(min_keep, recent.size() / 2);
    const size_t fold_count = recent.size() - keep;
    if (fold_count == 0) return out;

    std::vector<ChatMessage> folded(recent.begin(), recent.begin() + fold_count);
    const std::string transcript = build_transcript(summary, folded);

    std::vector<ChatMessage> req;
    {
        ChatMessage sys;
        sys.role = "system";
        sys.content = "Summarize the conversation below into a short, dense paragraph "
                      "that preserves facts, decisions, and stated preferences. Write "
                      "it as neutral background notes, not a transcript. No preamble.";
        req.push_back(std::move(sys));
        ChatMessage usr;
        usr.role = "user";
        usr.content = transcript;
        req.push_back(std::move(usr));
    }

    std::string new_summary;
    try {
        CompletionResponse resp = llm.complete(req, json::array());
        new_summary = resp.content;
    } catch (const std::exception& e) {
        std::cerr << "[context_compressor] summarization failed, skipping: "
                  << e.what() << "\n";
        return out;
    }
    if (new_summary.empty()) return out;

    memory.set_summary(session, agent, new_summary);
    memory.prune_turns(session, static_cast<int>(keep));

    recent.erase(recent.begin(), recent.begin() + fold_count);
    summary = new_summary;

    out.compressed    = true;
    out.turns_folded   = static_cast<int>(fold_count);
    out.summary_preview = new_summary.substr(0, 200);
    return out;
}
