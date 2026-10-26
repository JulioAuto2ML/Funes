// =============================================================================
// src/core/tool_log.cpp — see tool_log.h
// =============================================================================

#include "tool_log.h"
#include "text_utils.h"
#include <algorithm>
#include <cctype>

namespace funes {

namespace {

using nlohmann::json;

bool is_secret_key(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* const kMarkers[] = {
        "password", "passwd", "secret", "token", "api_key", "apikey",
        "authorization", "cookie", "credential", "private_key",
    };
    for (const char* m : kMarkers)
        if (key.find(m) != std::string::npos) return true;
    return false;
}

json redact(const json& j) {
    if (j.is_object()) {
        json out = json::object();
        for (auto it = j.begin(); it != j.end(); ++it)
            out[it.key()] = is_secret_key(it.key()) ? json("***") : redact(it.value());
        return out;
    }
    if (j.is_array()) {
        json out = json::array();
        for (const auto& v : j) out.push_back(redact(v));
        return out;
    }
    return j;
}

// One log line per call: control characters (a multi-line error, a stack
// trace) become spaces.
std::string one_line(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return s;
}

std::string clip(std::string s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    truncate_utf8_safe(s, max_bytes);
    return s + "…";
}

} // namespace

std::string summarize_tool_args(const json& args, size_t max_bytes) {
    return clip(one_line(dump_safe(redact(args))), max_bytes);
}

std::string format_tool_call_line(const std::string& agent, int depth,
                                  const std::string& key,
                                  const std::string& args_summary,
                                  ToolCallOutcome outcome,
                                  const std::string& detail,
                                  size_t max_detail_bytes) {
    std::string line = "[tool] " + agent + " (depth " + std::to_string(depth) + ") "
                     + key + " args=" + args_summary + " → ";
    switch (outcome) {
        case ToolCallOutcome::Ok:               line += "ok"; break;
        case ToolCallOutcome::BudgetRefused:    line += "refused: budget spent"; break;
        case ToolCallOutcome::PermissionDenied: line += "refused: permissions"; break;
        case ToolCallOutcome::RepeatRefused:    line += "refused: identical repeat"; break;
        case ToolCallOutcome::Error:
            line += "error: " + clip(one_line(detail), max_detail_bytes);
            break;
    }
    // For the two refusals `detail` names what refused it (which key, which
    // limit) — short and ours, not tool output.
    if (outcome != ToolCallOutcome::Ok && outcome != ToolCallOutcome::Error && !detail.empty())
        line += " (" + clip(one_line(detail), max_detail_bytes) + ")";
    return line + "\n";
}

} // namespace funes
