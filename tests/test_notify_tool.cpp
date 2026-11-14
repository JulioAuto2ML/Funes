// =============================================================================
// tests/test_notify_tool.cpp — notify_user
// =============================================================================
// The tool a scheduled reminder uses to reach its owner from Funes's own
// WhatsApp number. What has to hold: the message goes to the jid mapped to the
// run's account and to nobody else — not to a number the model names, not to
// the admin when the caller has none mapped — and every failure is a tool
// error the model can report, not a silent "ok" (the 2026-11-14 finding: a
// reminder job whose agent had no way to send anything ran "ok" and reached
// nobody).

#include "effects.h"
#include "permissions.h"
#include "tools/notify_tool.h"
#include <iostream>
#include <map>
#include <string>
#include <vector>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

struct Sent { std::string jid, text; };

struct Fixture {
    std::map<int64_t, std::string> jids{{1, "111@lid"}, {2, "222@lid"}};
    std::vector<Sent> sent;
    std::string bridge_error;   // what send answers; "" = delivered
    ToolRegistry reg;

    Fixture() {
        NotifyChannel ch;
        ch.jid_for = [this](int64_t uid) {
            auto it = jids.find(uid);
            return it == jids.end() ? std::string() : it->second;
        };
        ch.send = [this](const std::string& jid, const std::string& text) {
            if (bridge_error.empty()) sent.push_back({jid, text});
            return bridge_error;
        };
        register_notify_tool(reg, ch);
    }

    ToolResult call(int64_t user_id, const json& args) {
        ToolContext ctx("funes", "cron-1-1", "", "", user_id);
        return reg.call("notify_user", args, ctx);
    }
};

} // namespace

int test_sends_to_the_callers_own_jid() {
    Fixture f;
    CHECK(f.reg.has("notify_user"));
    auto r = f.call(2, {{"message", "Tomá el Relvar"}});
    CHECK(!r.error);
    CHECK(f.sent.size() == 1);
    CHECK(f.sent[0].jid == "222@lid");
    CHECK(f.sent[0].text == "Tomá el Relvar");
    return 0;
}

// The schema has no recipient. A model that adds one anyway still reaches only
// the caller: the tool is "tell my person", not "send a WhatsApp".
int test_a_recipient_argument_is_ignored() {
    Fixture f;
    auto r = f.call(2, {{"message", "hola"}, {"recipient", "111@lid"}, {"jid", "5491100000000"}});
    CHECK(!r.error);
    CHECK(f.sent.size() == 1);
    CHECK(f.sent[0].jid == "222@lid");
    return 0;
}

// No mapping means no message, never a fallback to another account's number.
int test_unmapped_account_is_an_error() {
    Fixture f;
    auto r = f.call(3, {{"message", "hola"}});
    CHECK(r.error);
    CHECK(r.text.find("jid-map") != std::string::npos);
    CHECK(f.sent.empty());
    return 0;
}

int test_message_is_validated() {
    Fixture f;
    CHECK(f.call(1, json::object()).error);
    CHECK(f.call(1, {{"message", ""}}).error);
    CHECK(f.call(1, {{"message", "   \n "}}).error);
    CHECK(f.call(1, {{"message", 42}}).error);
    CHECK(f.call(1, {{"message", std::string(kMaxNotifyChars + 1, 'x')}}).error);
    CHECK(f.sent.empty());
    CHECK(!f.call(1, {{"message", std::string(kMaxNotifyChars, 'x')}}).error);
    return 0;
}

// A bridge that is down or refuses must surface as an error, so the job's run
// is not recorded as having reminded anyone.
int test_bridge_failure_is_an_error() {
    Fixture f;
    f.bridge_error = "connection refused";
    auto r = f.call(1, {{"message", "hola"}});
    CHECK(r.error);
    CHECK(r.text.find("connection refused") != std::string::npos);
    return 0;
}

// It sends a message, so a tainted interactive run proposes it (effects.h);
// a scheduled job is exempt, which is where reminders run.
int test_is_external_and_not_privileged() {
    CHECK(funes::builtin_effect("notify_user") == funes::Effect::External);
    auto member = funes::Permissions::parse("", false);
    CHECK(member.allows_tool("notify_user"));
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_sends_to_the_callers_own_jid();
    rc |= test_a_recipient_argument_is_ignored();
    rc |= test_unmapped_account_is_an_error();
    rc |= test_message_is_validated();
    rc |= test_bridge_failure_is_an_error();
    rc |= test_is_external_and_not_privileged();
    if (rc == 0) std::cout << "test_notify_tool: all passed\n";
    return rc;
}
