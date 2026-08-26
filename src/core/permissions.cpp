// =============================================================================
// src/core/permissions.cpp — per-user agent and tool allowlists
// =============================================================================

#include "permissions.h"
#include "json.hpp"
#include <algorithm>
#include <iostream>

using json = nlohmann::json;

namespace funes {
namespace {

// Tools a member should not get simply because nobody wrote a permissions
// blob for them. execute_shell is real code execution with the Funes
// process's own rights; create_agent and create_tool write into the install
// itself, which outlives the conversation that asked for them.
const char* const PRIVILEGED[] = {"execute_shell", "create_agent", "create_tool"};

} // namespace

bool Permissions::is_privileged_tool(const std::string& tool) {
    for (const char* p : PRIVILEGED)
        if (tool == p) return true;
    return false;
}

Permissions Permissions::unrestricted() {
    Permissions p;
    p.admin_ = true;
    return p;
}

Permissions Permissions::parse(const std::string& json_text, bool is_admin) {
    Permissions p;
    p.admin_ = is_admin;
    if (is_admin || json_text.empty()) return p;

    json doc;
    try {
        doc = json::parse(json_text);
    } catch (const std::exception& e) {
        std::cerr << "[permissions] ignoring malformed permissions blob ("
                  << e.what() << "); falling back to member defaults\n";
        return p;
    }
    if (!doc.is_object()) {
        std::cerr << "[permissions] permissions must be a JSON object; "
                     "falling back to member defaults\n";
        return p;
    }

    // Each field is validated independently: a broken "tools" map should not
    // discard a perfectly good "agents" list.
    if (doc.contains("agents") && doc["agents"].is_array()) {
        for (const auto& a : doc["agents"])
            if (a.is_string()) p.agents_.push_back(a.get<std::string>());
    }
    if (doc.contains("tools") && doc["tools"].is_object()) {
        for (const auto& [name, allowed] : doc["tools"].items())
            if (allowed.is_boolean()) p.tools_[name] = allowed.get<bool>();
    }
    return p;
}

bool Permissions::allows_agent(const std::string& agent) const {
    if (admin_) return true;
    // Empty means unrestricted, not "none" — see the header. Writing
    // `agents: []` and locking someone out of everything would be a
    // surprising way to spell that.
    if (agents_.empty()) return true;
    return std::find(agents_.begin(), agents_.end(), agent) != agents_.end();
}

bool Permissions::allows_tool(const std::string& tool) const {
    if (admin_) return true;
    auto it = tools_.find(tool);
    if (it != tools_.end()) return it->second;   // explicit entry is final
    return !is_privileged_tool(tool);
}

std::vector<std::string> Permissions::filter_tools(
    const std::vector<std::string>& agent_tools,
    const std::vector<std::string>& all_tools) const {
    // An agent with no `tools:` list allows everything registered, so that
    // case has to be expanded before it can be narrowed — otherwise "empty"
    // would keep meaning "everything" after filtering and a member would
    // acquire the privileged tools through any such agent.
    const std::vector<std::string>& source = agent_tools.empty() ? all_tools : agent_tools;

    std::vector<std::string> out;
    out.reserve(source.size());
    for (const auto& t : source)
        if (allows_tool(t)) out.push_back(t);
    return out;
}

} // namespace funes
