// =============================================================================
// src/core/permissions.h — what one account is allowed to do
// =============================================================================
//
// Phase 4 of docs/dev-plan-users-permissions.md. Deliberately not RBAC: two
// roles, plus an optional per-user allowlist of agents and tools, stored as
// JSON in users.permissions.
//
//   {
//     "agents": ["funes", "researcher"],
//     "tools":  {"execute_shell": false, "web_search": true}
//   }
//
// Semantics, which are worth stating because "absent" means different things
// for the two fields:
//
//   agents  — absent or empty means every agent. A non-empty list is an
//             allowlist.
//   tools   — an entry is the final word for that tool. A tool with no entry
//             falls back to whether it is *privileged*: ordinary tools are
//             allowed, privileged ones (shell, agent/tool creation) are not.
//             That makes the plan's "chat + web search + files, no
//             shell/agent-creation" the behaviour of an empty object, so a new
//             member needs no permissions written out at all.
//
// An admin bypasses both. The role is the coarse switch; this is the fine one.
//
// Permissions restrict; they never widen. The result is always intersected
// with the agent's own `tools:` list, so granting a user `execute_shell` does
// not give it to them through an agent that was never given the tool.
// =============================================================================

#pragma once
#include <map>
#include <string>
#include <vector>

namespace funes {

class Permissions {
public:
    // Everything allowed — admins, and internal callers with no user context.
    static Permissions unrestricted();

    // Parses the users.permissions blob. Malformed JSON is not fatal: it
    // yields the defaults (which already deny the privileged tools) and logs,
    // because the alternative — refusing everything — locks a real person out
    // of their own assistant over a stray comma.
    static Permissions parse(const std::string& json_text, bool is_admin);

    bool allows_agent(const std::string& agent) const;
    bool allows_tool(const std::string& tool) const;

    // Filters an agent's tool allowlist down to what this user may use.
    // Empty input means "the agent allows everything", which still has to be
    // resolved against `all_tools` before it can be narrowed.
    std::vector<std::string> filter_tools(const std::vector<std::string>& agent_tools,
                                          const std::vector<std::string>& all_tools) const;

    bool is_admin() const { return admin_; }

    // Tools that do something a member should not get by default: real code
    // execution, and writing new agents or tools into the install.
    static bool is_privileged_tool(const std::string& tool);

private:
    bool admin_ = false;
    std::vector<std::string>    agents_;  // empty = every agent
    std::map<std::string, bool> tools_;   // absent = fall back to privilege
};

} // namespace funes
