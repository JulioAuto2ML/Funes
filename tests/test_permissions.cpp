// =============================================================================
// tests/test_permissions.cpp — per-user agent and tool allowlists
// =============================================================================
// The cases that matter are the defaults (a member with no permissions
// written out must still be usable, and must still not get a shell) and the
// direction of the intersection: permissions restrict, they never widen.

#include "permissions.h"
#include <iostream>
#include <string>
#include <vector>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

using funes::Permissions;

static bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

int test_admin_bypasses_everything() {
    auto p = Permissions::parse(R"({"agents":["nothing"],"tools":{"web_search":false}})", true);
    CHECK(p.is_admin());
    CHECK(p.allows_agent("curator"));
    CHECK(p.allows_tool("web_search"));
    CHECK(p.allows_tool("execute_shell"));
    CHECK(p.allows_tool("create_agent"));
    return 0;
}

int test_member_defaults() {
    // The plan's stated default for a new member: chat + web search + files,
    // no shell, no agent/tool creation — and that must be what an empty
    // permissions object produces, so nobody has to write it out per account.
    auto p = Permissions::parse("{}", false);
    CHECK(!p.is_admin());

    CHECK(p.allows_tool("web_search"));
    CHECK(p.allows_tool("web_fetch"));
    CHECK(p.allows_tool("read_file"));
    CHECK(p.allows_tool("write_file"));
    CHECK(p.allows_tool("remember"));
    CHECK(p.allows_tool("delegate_to_agent"));

    CHECK(!p.allows_tool("execute_shell"));
    CHECK(!p.allows_tool("create_agent"));
    CHECK(!p.allows_tool("create_tool"));

    // No agents list means every agent.
    CHECK(p.allows_agent("funes"));
    CHECK(p.allows_agent("curator"));
    return 0;
}

int test_explicit_entries_win() {
    auto p = Permissions::parse(
        R"({"tools":{"execute_shell":true,"web_search":false}})", false);
    CHECK(p.allows_tool("execute_shell"));    // privileged, but granted
    CHECK(!p.allows_tool("web_search"));      // ordinary, but revoked
    CHECK(p.allows_tool("read_file"));        // untouched, still default
    return 0;
}

int test_agent_allowlist() {
    auto p = Permissions::parse(R"({"agents":["funes","researcher"]})", false);
    CHECK(p.allows_agent("funes"));
    CHECK(p.allows_agent("researcher"));
    CHECK(!p.allows_agent("operator"));
    CHECK(!p.allows_agent("curator"));
    CHECK(!p.allows_agent(""));

    // An empty list is "no restriction", not "no agents" — otherwise writing
    // `agents: []` would silently lock someone out of everything.
    auto empty = Permissions::parse(R"({"agents":[]})", false);
    CHECK(empty.allows_agent("anything"));
    return 0;
}

int test_filter_tools_only_narrows() {
    const std::vector<std::string> all = {
        "web_search", "web_fetch", "read_file", "write_file", "execute_shell", "remember"};

    // The agent grants four; the user is denied one of them and granted one
    // the agent never had. The result must be the intersection.
    auto p = Permissions::parse(
        R"({"tools":{"web_fetch":false,"execute_shell":true}})", false);

    auto got = p.filter_tools({"web_search", "web_fetch", "read_file"}, all);
    CHECK(has(got, "web_search"));
    CHECK(has(got, "read_file"));
    CHECK(!has(got, "web_fetch"));        // revoked by the user's permissions
    CHECK(!has(got, "execute_shell"));    // granted to the user, but not to this agent
    CHECK(got.size() == 2);
    return 0;
}

int test_filter_tools_with_open_agent() {
    // An agent with an empty `tools:` list allows every registered tool. The
    // user's permissions still apply on top — this is the case where a member
    // must not silently acquire the shell.
    const std::vector<std::string> all = {"web_search", "execute_shell", "create_agent"};
    auto member = Permissions::parse("{}", false);

    auto got = member.filter_tools({}, all);
    CHECK(has(got, "web_search"));
    CHECK(!has(got, "execute_shell"));
    CHECK(!has(got, "create_agent"));

    // An admin through the same path keeps everything.
    auto admin = Permissions::unrestricted();
    CHECK(admin.filter_tools({}, all).size() == all.size());
    return 0;
}

int test_malformed_json_falls_back_to_defaults() {
    // Not fatal, and specifically not "deny everything": a stray comma must
    // not lock someone out of their own assistant. But it must also not
    // quietly hand out the privileged tools.
    for (const char* bad : {"", "{", "not json", "[]", "null", R"({"tools":42})",
                            R"({"agents":"funes"})"}) {
        auto p = Permissions::parse(bad, false);
        CHECK(p.allows_tool("web_search"));
        CHECK(p.allows_agent("funes"));
        CHECK(!p.allows_tool("execute_shell"));
        CHECK(!p.allows_tool("create_agent"));
    }
    return 0;
}

int test_unrestricted() {
    auto p = Permissions::unrestricted();
    CHECK(p.allows_agent("anything"));
    CHECK(p.allows_tool("execute_shell"));
    CHECK(p.allows_tool("create_tool"));
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_admin_bypasses_everything();
    rc |= test_member_defaults();
    rc |= test_explicit_entries_win();
    rc |= test_agent_allowlist();
    rc |= test_filter_tools_only_narrows();
    rc |= test_filter_tools_with_open_agent();
    rc |= test_malformed_json_falls_back_to_defaults();
    rc |= test_unrestricted();
    if (rc == 0) std::cout << "test_permissions: all passed\n";
    return rc;
}
