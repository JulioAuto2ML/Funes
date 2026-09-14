// =============================================================================
// tests/test_agent_roster.cpp — what a restricted account is told
// =============================================================================
// The roster used to be every loaded agent, unfiltered. That was correct while
// every account could use every agent, and became a bug the moment one could
// not: the model reads about gmail-assistant, delegates to it, is refused, and
// then has to explain a failure whose cause is not in its context. What it does
// with that is improvise — blame a server setting, suggest an environment
// variable, or try again.
//
// So the assertions here are about the *content* of what the model is told,
// which is unusual for a test and is the point: these strings are the whole
// mechanism. Two reasons an agent can be unavailable, and they need opposite
// advice — one an admin can fix, one no admin can.

#include "agent_roster.h"
#include <iostream>
#include <string>
#include <vector>

using funes::Permissions;
using funes::RosterEntry;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static std::vector<RosterEntry> table() {
    return {
        {"funes",            "The orchestrator.",        "",                  ""},
        {"researcher",       "Deep web research.",       "Give it a topic.",  ""},
        {"gmail-assistant",  "Reads and drafts mail.",   "",
         "the installation's Gmail mailbox"},
        {"curator",          "Publishes the newsletter.", "",
         "the installation's subscriber list and sending account"},
    };
}

int test_an_admin_sees_everything_and_no_denied_block() {
    const auto roster = funes::build_roster(table(), Permissions::unrestricted(), "funes");

    CHECK(roster.available.find("researcher") != std::string::npos);
    CHECK(roster.available.find("gmail-assistant") != std::string::npos);
    CHECK(roster.available.find("Give it a topic.") != std::string::npos);  // delegation notes kept
    CHECK(roster.available.find("funes") == std::string::npos);             // excluded from its own roster

    // Nothing to say, so nothing is said: an admin's prompt must not spend
    // context on a block explaining that they can use everything.
    CHECK(roster.denied.empty());
    CHECK(funes::denied_agents_block(roster.denied).empty());
    return 0;
}

int test_a_restricted_account_is_told_which_and_why() {
    const auto perms = Permissions::parse(R"({"agents":["funes","researcher"]})",
                                          /*is_admin=*/false);
    const auto roster = funes::build_roster(table(), perms, "funes");

    CHECK(roster.available.find("researcher") != std::string::npos);
    CHECK(roster.available.find("gmail-assistant") == std::string::npos);

    // Named rather than omitted. An agent the model has never heard of gets
    // "Funes cannot read mail"; a named one it may not use gets "this account
    // cannot", which is true and is something the person can act on.
    CHECK(roster.denied.find("gmail-assistant") != std::string::npos);
    CHECK(roster.denied.find("curator") != std::string::npos);

    // The two reasons, and the advice they imply, must not be interchangeable.
    const auto perms2 = Permissions::parse(R"({"agents":["funes"]})", false);
    const auto only_allowlist = funes::build_roster(
        {{"operator", "Files and shell.", "", ""}}, perms2, "funes");
    CHECK(only_allowlist.denied.find("administrator can grant it") != std::string::npos);

    const auto shared = funes::build_roster(
        {{"gmail-assistant", "Mail.", "", "the installation's Gmail mailbox"}},
        perms2, "funes");
    CHECK(shared.denied.find("belongs to the installation") != std::string::npos);
    CHECK(shared.denied.find("not something an administrator can grant") != std::string::npos ||
          shared.denied.find("is not something an administrator can grant") != std::string::npos ||
          shared.denied.find("not something\nan administrator") != std::string::npos ||
          shared.denied.find("an administrator can grant per account") != std::string::npos);
    // The one thing it must never say about a shared-identity agent.
    CHECK(shared.denied.find("an administrator can grant it") == std::string::npos);
    return 0;
}

int test_the_prompt_block_forbids_the_two_wrong_answers() {
    const auto perms = Permissions::parse(R"({"agents":["funes"]})", false);
    const auto roster = funes::build_roster(table(), perms, "funes");
    const std::string block = funes::denied_agents_block(roster.denied);

    CHECK(!block.empty());
    CHECK(block.find("## Agents this account may not use") != std::string::npos);
    // Every prompt written before per-user permissions existed blames the
    // server switch; a member denied a tool was once told to set
    // FUNES_ALLOW_SHELL, which was already set and would not have helped.
    // The agent half of that instruction has to be as explicit.
    CHECK(block.find("Never blame a server setting") != std::string::npos);
    CHECK(block.find("never suggest changing one") != std::string::npos);
    return 0;
}

int test_the_delegation_refusal_carries_the_same_distinction() {
    const std::string plain = funes::delegation_refusal("operator", "");
    CHECK(plain.find("operator") != std::string::npos);
    CHECK(plain.find("administrator can grant it") != std::string::npos);

    // This string is frequently the only thing the calling model relays to the
    // person, so it has to be right on its own, not merely consistent with a
    // prompt block that may have been compressed away by then.
    const std::string shared =
        funes::delegation_refusal("gmail-assistant", "the installation's Gmail mailbox");
    CHECK(shared.find("cannot be given it") != std::string::npos);
    CHECK(shared.find("the installation's Gmail mailbox") != std::string::npos);
    CHECK(shared.find("handing over someone else's data") != std::string::npos);
    CHECK(shared.find("Do not suggest a configuration change") != std::string::npos);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_an_admin_sees_everything_and_no_denied_block();
    rc |= test_a_restricted_account_is_told_which_and_why();
    rc |= test_the_prompt_block_forbids_the_two_wrong_answers();
    rc |= test_the_delegation_refusal_carries_the_same_distinction();
    if (rc == 0) std::cout << "test_agent_roster: all passed\n";
    return rc;
}
