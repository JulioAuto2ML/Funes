// =============================================================================
// src/core/context_budget.cpp — see context_budget.h
// =============================================================================

#include "context_budget.h"
#include "context_compressor.h"   // estimate_tokens
#include "text_utils.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>

namespace funes {

namespace {

// What survives of a shortened message. Enough to say what it was — a tool
// result's head carries its result_id when it was stored — not enough to
// matter to the budget.
constexpr size_t kKeepHead = 400;
// Below this there is nothing worth taking: the marker would be most of it.
constexpr size_t kMinShrinkable = 600;

int estimate(const std::vector<ChatMessage>& history, int overhead) {
    return estimate_tokens(history) + overhead;
}

void shorten(std::string& text) {
    const size_t original = text.size();
    truncate_utf8_safe(text, kKeepHead);
    text += "\n[… " + std::to_string(original - text.size()) +
            " characters elided to keep this run inside the context window]";
}

// Index of the latest assistant message: it and everything after it are the
// step in progress. history.size() when there is none.
size_t protected_tail(const std::vector<ChatMessage>& history) {
    for (size_t i = history.size(); i-- > 0;)
        if (history[i].role == "assistant") return i;
    return history.size();
}

// The newest tool result and the newest call of each tool, by message index.
// Oldest-first alone would take curator's harvest pool — the one the issue is
// published from — before the read_result pages fetched after it, so these go
// only once everything else has.
std::set<size_t> newest_per_tool(const std::vector<ChatMessage>& history) {
    std::map<std::string, size_t> result_at, call_at;
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& m = history[i];
        if (m.role == "tool") result_at[m.name] = i;
        if (m.role == "assistant" && m.tool_calls.is_array())
            for (const auto& call : m.tool_calls)
                call_at[call.value("function", nlohmann::json::object()).value("name", "")] = i;
    }
    std::set<size_t> newest;
    for (const auto& [name, i] : result_at) newest.insert(i);
    for (const auto& [name, i] : call_at)   newest.insert(i);
    return newest;
}

}  // namespace

TrimOutcome trim_to_fit(std::vector<ChatMessage>& history, int budget_tokens,
                        int overhead_tokens) {
    TrimOutcome out;
    out.before = estimate(history, overhead_tokens);
    out.after  = out.before;
    out.fits   = out.before <= budget_tokens;
    if (out.fits) return out;

    const size_t tail = protected_tail(history);
    auto fits = [&] {
        out.after = estimate(history, overhead_tokens);
        out.fits  = out.after <= budget_tokens;
        return out.fits;
    };

    // Each pass walks oldest → newest and stops the moment the estimate fits,
    // so the newest material is the last to go.
    for (size_t i = 0; i < tail; ++i) {
        auto& m = history[i];
        if (m.role == "system" || m.role == "user" || m.images.empty()) continue;
        out.images += static_cast<int>(m.images.size());
        m.images.clear();
        if (fits()) return out;
    }
    const std::set<size_t> newest = newest_per_tool(history);
    auto shrink_results = [&](bool newest_too) {
        for (size_t i = 0; i < tail; ++i) {
            auto& m = history[i];
            if (m.role != "tool" || m.content.size() < kMinShrinkable) continue;
            if (!newest_too && newest.count(i)) continue;
            shorten(m.content);
            ++out.tool_results;
            if (fits()) return true;
        }
        return false;
    };
    auto shrink_calls = [&](bool newest_too) {
        for (size_t i = 0; i < tail; ++i) {
            auto& m = history[i];
            if (m.role != "assistant" || !m.tool_calls.is_array()) continue;
            if (!newest_too && newest.count(i)) continue;
            bool changed = false;
            for (auto& call : m.tool_calls) {
                if (!call.contains("function") || !call["function"].contains("arguments")) continue;
                auto& args = call["function"]["arguments"];
                const size_t size = args.is_string() ? args.get_ref<const std::string&>().size()
                                                     : args.dump().size();
                if (size < kMinShrinkable) continue;
                // Still a JSON object, as the OpenAI wire format requires of a
                // call's arguments — a raw marker string would get the whole
                // history refused.
                args = nlohmann::json{{"_elided", std::to_string(size) +
                    " characters of arguments, dropped to keep this run inside the "
                    "context window"}}.dump();
                changed = true;
            }
            if (changed) {
                ++out.tool_calls;
                if (fits()) return true;
            }
        }
        return false;
    };
    if (shrink_results(false) || shrink_calls(false) ||
        shrink_results(true)  || shrink_calls(true))
        return out;
    for (size_t i = 0; i < tail; ++i) {
        auto& m = history[i];
        if (m.role != "assistant" || m.content.size() < kMinShrinkable) continue;
        shorten(m.content);
        ++out.assistant_texts;
        if (fits()) return out;
    }
    fits();
    return out;
}

bool is_context_overflow(const std::string& error_text) {
    std::string lower = error_text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* const kMarkers[] = {
        "exceed_context_size",              // llama.cpp error type
        "exceeds the available context",    // llama.cpp message
        "context_length_exceeded",          // OpenAI error code
        "maximum context length",           // OpenAI / vLLM message
        "prompt is too long",               // Anthropic
    };
    for (const char* marker : kMarkers)
        if (lower.find(marker) != std::string::npos) return true;
    return false;
}

int reported_prompt_tokens(const std::string& error_text) {
    static const std::regex kPatterns[] = {
        std::regex(R"re("n_prompt_tokens"\s*:\s*(\d+))re"),
        std::regex(R"re(request \((\d+) tokens\))re"),
        std::regex(R"re(resulted in (\d+) tokens)re"),
        std::regex(R"re(prompt is too long:\s*(\d+) tokens)re"),
    };
    // Error bodies are short, but a regex over an unbounded string is how
    // llm_client once blew the stack; only the head can hold the figure.
    const std::string head = error_text.substr(0, 4096);
    std::smatch match;
    for (const auto& pattern : kPatterns)
        if (std::regex_search(head, match, pattern)) {
            try { return std::stoi(match[1].str()); } catch (...) { return 0; }
        }
    return 0;
}

}  // namespace funes
