// =============================================================================
// tests/test_fix_incident.cpp — knowing what it does not know (7.23)
// =============================================================================
// The 2026-11-14 evening, as tests. Asked to retry one LinkedIn post, the
// curator proposed re-sending the newsletter, described it as "the post is
// ready", and the approval was answered "Listo" although nothing was sent; the
// TV's sandbox refusal read as a file-permission problem to three agents.
//
//   - a proposal is described by the code, from the tool and its arguments
//   - a run lists the proposals it (and its delegates) made, by session
//   - fix_incident does what heal.yaml says: proposes its call, passes on its
//     explanation, or says that no fix is known — never anything else
//   - the newsletter probe's per-post key resolves to retry_post's argument
//   - a sandboxed script's permission error says what it probably is

#include "heal.h"
#include "incidents.h"
#include "proposal_text.h"
#include "proposals.h"
#include "script_library.h"
#include "tools.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const fs::path kBase = fs::temp_directory_path() / "funes_test_fix_incident";

std::string fresh(const std::string& name) {
    const fs::path p = kBase / name;
    fs::remove(p);
    fs::remove(p.string() + "-wal");
    fs::remove(p.string() + "-shm");
    return p.string();
}

const char* kPolicy = R"yaml(
rules:
  - when: "probe_newsletter:post.*"
    level: propose
    action: reintentar el post de LinkedIn
    agent: sysadmin
    tool: run_script
    args: {name: retry_post, arguments: {post: "${match}"}}
    undo: none
    explain: "Si es el token, renovalo antes de aprobar."
  - when: "probe_services:llm"
    level: explain
    explain: "llama-server no responde: ~/launch_llama.sh restart big"
)yaml";

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

} // namespace

int test_describe_call() {
    const nlohmann::json resend = {{"publication", "ai-pulse-es"}, {"date", "2026-11-14"}, {"items", nlohmann::json::array()}};
    const std::string es = funes::describe_call("publish_issue", resend, true);
    CHECK(contains(es, "enviar el newsletter ai-pulse-es (2026-11-14) a todos sus suscriptores"));
    CHECK(contains(funes::describe_call("publish_issue", {{"publication", "x"}, {"dry_run", true}}, false), "without sending"));
    CHECK(contains(funes::describe_call("run_script", {{"name", "retry_post"}, {"arguments", {{"post", "ai-pulse-es.5"}}}}, true),
                   "correr el script «retry_post»"));
    CHECK(contains(funes::describe_call("cancel_job", {{"id", 3}, {"name", "AI Pulse ES"}}, true), "borrar el job programado #3"));
    // Anything else: its name and its arguments, a credential masked.
    const std::string other = funes::describe_call("mystery", {{"api_key", "sk-123"}, {"n", 2}}, false);
    CHECK(contains(other, "mystery") && contains(other, "\"n\":2") && !contains(other, "sk-123"));
    return 0;
}

int test_footer_lists_what_the_run_proposed() {
    funes::ProposalStore store(fresh("p.db"));
    store.create(1, "curator", "run_script", {{"name", "x"}}, "old-session", "web", "external");
    const int64_t before = store.max_id();
    const int64_t mine = store.create(1, "curator", "publish_issue", {{"publication", "ai-pulse-es"}},
                                      "wa", "tool:incidents", "external");
    store.create(1, "home", "run_script", {{"name", "tv_art"}}, "other-session", "web", "external");
    store.create(2, "curator", "publish_issue", {{"publication", "ai-pulse"}}, "wa", "web", "external");
    const auto made = store.made_since(1, "wa", before);
    CHECK(made.size() == 1 && made[0].id == mine);
    const std::string footer = funes::proposals_footer(made, true);
    CHECK(contains(footer, "Propuesta #" + std::to_string(mine) + " (curator)"));
    CHECK(contains(footer, "a todos sus suscriptores"));
    CHECK(contains(footer, "«aprobar " + std::to_string(mine) + "»"));
    CHECK(funes::proposals_footer({}, true).empty());
    return 0;
}

int test_rule_for_fills_the_match() {
    funes::heal::Policy p;
    CHECK(funes::heal::parse_policy(kPolicy, p).empty());
    const auto r = funes::heal::rule_for(p, "probe_newsletter:post.ai-pulse-es.5");
    CHECK(r && r->args["arguments"]["post"] == "ai-pulse-es.5");
    CHECK(!funes::heal::rule_for(p, "probe_newsletter:linkedin_token"));
    // An uncertain post is not a "post." key: no retry is proposed for it.
    CHECK(!funes::heal::rule_for(p, "probe_newsletter:post_unsure.ai-pulse-es.5"));
    // The probe failing to run is not a post.
    CHECK(!funes::heal::rule_for(p, "probe_newsletter:(run)"));
    return 0;
}

int test_fix_incident() {
    const std::string policy_path = (kBase / "heal.yaml").string();
    { std::ofstream(policy_path) << kPolicy; }
    funes::incidents::IncidentStore incidents(fresh("i.db"));
    funes::ProposalStore proposals(fresh("q.db"));
    ToolRegistry reg;
    register_incident_tools(reg, &incidents, &proposals, policy_path);
    CHECK(reg.has("fix_incident"));
    ToolContext ctx{"funes", "wa-session"};
    ctx.user_id = 1;

    const int64_t now = 1794689526;
    auto open = [&](const std::string& key) {
        incidents.observe({key, false, "warning", "down: " + key, "ConnectionResetError(104)"}, 1, now);
        return incidents.open_for(key)->id;
    };

    // A rule with a call: proposed in this conversation, never run.
    const int64_t post = open("probe_newsletter:post.ai-pulse-es.5");
    ToolResult r = reg.call("fix_incident", {{"id", post}}, ctx);
    CHECK(!r.error && r.proposal_id > 0);
    CHECK(contains(r.text, "PROPOSED") && contains(r.text, "NOT RUN") && contains(r.text, "renovalo"));
    const auto p = proposals.get(1, r.proposal_id);
    CHECK(p && p->session == "wa-session" && p->agent == "sysadmin" && p->tool == "run_script");
    CHECK(p->args["arguments"]["post"] == "ai-pulse-es.5");

    // A rule that only explains: its words, nothing proposed.
    const int64_t llm = open("probe_services:llm");
    r = reg.call("fix_incident", {{"id", llm}}, ctx);
    CHECK(!r.error && r.proposal_id == 0 && contains(r.text, "launch_llama.sh") && contains(r.text, "no automatic fix"));

    // Nothing known: says so, with the evidence, and proposes nothing.
    const int64_t tv = open("jobs:5");
    r = reg.call("fix_incident", {{"id", tv}}, ctx);
    CHECK(!r.error && r.proposal_id == 0 && contains(r.text, "NO KNOWN FIX") && contains(r.text, "ConnectionResetError"));

    // Resolved, or not there.
    incidents.observe({"jobs:5", true, "", "ok", ""}, 1, now + 60);
    CHECK(contains(reg.call("fix_incident", {{"id", tv}}, ctx).text, "already resolved"));
    CHECK(reg.call("fix_incident", {{"id", 9999}}, ctx).error);
    CHECK(reg.call("fix_incident", nlohmann::json::object(), ctx).error);
    return 0;
}

int test_sandbox_hint() {
    const std::string tv = "exit_code: 1\nstderr:\ntv_art: failed: PermissionError: [Errno 13] Permission denied\n";
    const std::string hint = funes::sandbox_refusal_hint("tv_art", tv);
    CHECK(contains(hint, "sandbox") && contains(hint, "script-learn tv_art") && contains(hint, "Do not retry"));
    CHECK(funes::sandbox_refusal_hint("tv_art", "exit_code: 1\nConnectionFailure: Websocket Time out").empty());
    return 0;
}

int main() {
    fs::create_directories(kBase);
    int rc = 0;
    rc |= test_describe_call();
    rc |= test_footer_lists_what_the_run_proposed();
    rc |= test_rule_for_fills_the_match();
    rc |= test_fix_incident();
    rc |= test_sandbox_hint();
    if (rc == 0) std::cout << "test_fix_incident: all passed\n";
    return rc;
}
