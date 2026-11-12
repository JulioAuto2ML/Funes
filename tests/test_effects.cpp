// tests/test_effects.cpp — the effects gate's pieces (effects.h, proposals.h)
//
// What a call's class is (built-ins listed, a script manifest or MCP server
// that does not say is external), what counts as the person answering a
// proposal (only the bare command — a sentence with the word in it reaches
// the model), and the store: owner-only, decided once, stale after a day.
// The gate itself, end to end through /api/chat, is in tests/integration.sh.

#include "agent_config.h"
#include "effects.h"
#include "proposals.h"
#include "script_library.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
using funes::Effect;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const fs::path kBase = fs::temp_directory_path() / ("funes_test_effects_" + std::to_string(getpid()));

int test_effect_names_round_trip() {
    for (Effect e : {Effect::None, Effect::Workspace, Effect::Host, Effect::External})
        CHECK(funes::parse_effect(funes::effect_name(e)) == e);
    CHECK(!funes::parse_effect("External"));
    CHECK(!funes::parse_effect(""));
    CHECK(!funes::needs_approval(Effect::None));
    CHECK(!funes::needs_approval(Effect::Workspace));
    CHECK(funes::needs_approval(Effect::Host));
    CHECK(funes::needs_approval(Effect::External));
    std::cout << "  ok: effect classes parse, print, and host/external need approval\n";
    return 0;
}

int test_builtin_effects() {
    CHECK(funes::builtin_effect("publish_issue") == Effect::External);
    CHECK(funes::builtin_effect("schedule_job") == Effect::External);   // no laundering via cron
    CHECK(funes::builtin_effect("run_job_now") == Effect::External);
    CHECK(funes::builtin_effect("execute_shell") == Effect::Host);
    CHECK(funes::builtin_effect("create_agent") == Effect::Host);
    CHECK(funes::builtin_effect("write_file") == Effect::Workspace);
    CHECK(funes::builtin_effect("web_fetch") == Effect::None);
    CHECK(funes::builtin_effect("delegate_to_agent") == Effect::None);   // the sub-run is gated
    std::cout << "  ok: built-ins that act are listed; reading and delegating are not\n";
    return 0;
}

int test_approval_commands() {
    auto a = funes::parse_approval("aprobar 12");
    CHECK(a && a->approve && a->id == 12);
    a = funes::parse_approval("  Approve #7. ");
    CHECK(a && a->approve && a->id == 7);
    a = funes::parse_approval("APRUEBO p-3!");
    CHECK(a && a->approve && a->id == 3);
    a = funes::parse_approval("rechazar 4");
    CHECK(a && !a->approve && a->id == 4);
    a = funes::parse_approval("reject 9");
    CHECK(a && !a->approve && a->id == 9);
    a = funes::parse_approval("Aprobar 5");
    CHECK(a && a->id == 5);
    a = funes::parse_approval("\xC3\x81probar 6");   // Á
    CHECK(a && a->id == 6);

    CHECK(!funes::parse_approval("approve the budget 2"));
    CHECK(!funes::parse_approval("aprobar"));
    CHECK(!funes::parse_approval("aprobar 0"));
    CHECK(!funes::parse_approval("aprobar 12 y mandalo"));
    CHECK(!funes::parse_approval("aprobar12"));
    CHECK(!funes::parse_approval("please approve 12"));
    CHECK(!funes::parse_approval("approved 12"));
    CHECK(!funes::parse_approval("aprobar 99999999999999999999"));
    std::cout << "  ok: only the bare command answers a proposal\n";
    return 0;
}

int test_store_is_owner_only_and_decided_once() {
    funes::ProposalStore store((kBase / "p.db").string());
    const int64_t id = store.create(1, "whatsapp-assistant", "send_message",
                                    {{"recipient", "123"}, {"message", "hola"}}, "s1",
                                    "tool:list_messages", "external");
    CHECK(id > 0);

    CHECK(!store.get(2, id));                                   // not yours = not there
    CHECK(store.decide(2, id, true) == funes::ProposalStore::Decision::NotFound);
    CHECK(store.pending(2).empty());

    auto p = store.get(1, id);
    CHECK(p && p->status == "pending" && p->tool == "send_message");
    CHECK(p->args["message"] == "hola");
    CHECK(store.pending(1).size() == 1);

    CHECK(store.decide(1, id, true) == funes::ProposalStore::Decision::Ok);
    CHECK(store.decide(1, id, true) == funes::ProposalStore::Decision::AlreadyDecided);
    CHECK(store.decide(1, id, false) == funes::ProposalStore::Decision::AlreadyDecided);
    store.finish(1, id, true, "sent");
    p = store.get(1, id);
    CHECK(p->status == "done" && p->result == "sent");
    CHECK(store.pending(1).empty());

    const int64_t r = store.create(1, "a", "speak", nlohmann::json::object(), "s", "", "external");
    CHECK(store.decide(1, r, false) == funes::ProposalStore::Decision::Ok);
    store.finish(1, r, true, "ran anyway?");                    // only an approved one finishes
    CHECK(store.get(1, r)->status == "rejected");
    std::cout << "  ok: a proposal is its owner's, decided once, finished only if approved\n";
    return 0;
}

int test_store_expires_after_a_day() {
    funes::ProposalStore store((kBase / "p.db").string());
    const int64_t id = store.create(1, "a", "speak", nlohmann::json::object(), "s", "", "external");
    store.set_now_for_test(std::time(nullptr) + funes::ProposalStore::kTtlSeconds + 10);
    CHECK(store.pending(1).empty());
    CHECK(store.decide(1, id, true) == funes::ProposalStore::Decision::Expired);
    CHECK(store.get(1, id)->status == "pending");
    std::cout << "  ok: a day-old proposal can no longer be approved\n";
    return 0;
}

int test_script_manifest_effects() {
    const fs::path lib = kBase / "lib";
    fs::create_directories(lib);
    std::ofstream(lib / "quiet.yaml") << "description: q\nrun: quiet.sh\neffects: none\n";
    std::ofstream(lib / "silent.yaml") << "description: s\nrun: silent.sh\n";
    std::ofstream(lib / "bad.yaml") << "description: b\nrun: bad.sh\neffects: loud\n";
    for (const char* f : {"quiet.sh", "silent.sh", "bad.sh"}) std::ofstream(lib / f) << "#!/bin/sh\n";

    funes::ScriptSpec spec;
    CHECK(funes::load_script(lib.string(), "quiet", spec).empty());
    CHECK(spec.effects == Effect::None);
    CHECK(funes::load_script(lib.string(), "silent", spec).empty());
    CHECK(spec.effects == Effect::External);                     // does not say → external
    CHECK(funes::load_script(lib.string(), "bad", spec).find("effects must be") != std::string::npos);
    std::cout << "  ok: a manifest's effects: is read; absent is external; a typo is refused\n";
    return 0;
}

int test_mcp_server_effects() {
    const fs::path f = kBase / "agent.yaml";
    std::ofstream(f) << "name: t\nsystem_prompt: x\nmcp_servers:\n"
                        "  - name: wa\n    command: wa\n"
                        "    effects: {default: none, send_message: external}\n"
                        "    own_data: [search_contacts]\n"
                        "  - name: other\n    command: other\n";
    const AgentConfig cfg = AgentConfig::from_file(f.string());
    CHECK(cfg.mcp_servers.size() == 2);
    CHECK(cfg.mcp_servers[0].effect_of("send_message") == Effect::External);
    CHECK(cfg.mcp_servers[0].effect_of("list_messages") == Effect::None);
    CHECK(cfg.mcp_servers[0].own_data.count("search_contacts") == 1);
    CHECK(cfg.mcp_servers[1].effect_of("anything") == Effect::External);   // no map → external

    std::ofstream(f) << "name: t\nsystem_prompt: x\nmcp_servers:\n"
                        "  - name: wa\n    command: wa\n    effects: {send_message: loud}\n";
    bool threw = false;
    try { AgentConfig::from_file(f.string()); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    std::cout << "  ok: an MCP server's effects map is read; no map is external throughout\n";
    return 0;
}

} // namespace

int main() {
    fs::create_directories(kBase);
    int rc = 0;
    rc |= test_effect_names_round_trip();
    rc |= test_builtin_effects();
    rc |= test_approval_commands();
    rc |= test_store_is_owner_only_and_decided_once();
    rc |= test_store_expires_after_a_day();
    rc |= test_script_manifest_effects();
    rc |= test_mcp_server_effects();
    fs::remove_all(kBase);
    if (rc == 0) std::cout << "effects: all tests passed\n";
    return rc;
}
