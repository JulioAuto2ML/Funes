// =============================================================================
// src/core/agent_roster.cpp
// =============================================================================

#include "agent_roster.h"

namespace funes {

Roster build_roster(const std::vector<RosterEntry>& entries,
                    const Permissions& perms, const std::string& exclude) {
    Roster out;
    for (const auto& e : entries) {
        if (e.name == exclude) continue;

        if (perms.allows_agent(e.name)) {
            out.available += "- " + e.name + ": " + e.description + "\n";
            if (!e.delegation_notes.empty())
                out.available += "  Note: " + e.delegation_notes + "\n";
            continue;
        }

        // A denied agent is still named. The alternative — omitting it — was
        // the old behaviour's other half: the person asks for their mail, the
        // model has never heard of an agent that reads mail, and answers that
        // Funes cannot do it at all. Naming it means the answer is "this
        // account can't", which is true and actionable.
        out.denied += "- " + e.name + " — ";
        out.denied += e.shared_identity.empty()
            ? "not on this account's agent allowlist; an administrator can grant it"
            : "works on " + e.shared_identity + ", which belongs to the "
              "installation rather than to this account — it is not something "
              "an administrator can grant per account";
        out.denied += "\n";
    }
    return out;
}

std::string denied_agents_block(const std::string& denied) {
    if (denied.empty()) return {};
    return "\n\n## Agents this account may not use\n" + denied +
           "\nDo not delegate to these and do not offer them. If the user asks "
           "for something one of them would do, say plainly that this account "
           "does not have access to it, and say which of the two reasons "
           "above applies — an administrator can grant the first kind and "
           "cannot grant the second. Never blame a server setting or an "
           "environment variable, and never suggest changing one: this is an "
           "account's access, and sending someone to edit configuration sends "
           "them to fix something that is not the cause.";
}

std::string delegation_refusal(const std::string& agent,
                               const std::string& shared_identity) {
    if (shared_identity.empty())
        return "This account does not have access to the agent '" + agent +
               "'. An administrator can grant it. Answer with what you can do "
               "yourself, or tell the user that this needs access they do not "
               "currently have.";

    return "This account does not have access to the agent '" + agent +
           "', and cannot be given it: " + agent + " works on " + shared_identity +
           ", which belongs to the installation rather than to this account. "
           "Tell the user plainly that this is not something their account can "
           "do and not something an administrator can grant them — it would "
           "mean handing over someone else's data. Do not suggest a "
           "configuration change.";
}

} // namespace funes
