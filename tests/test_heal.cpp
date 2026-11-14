// =============================================================================
// tests/test_heal.cpp — the heal policy and the healer (self-healing H2)
// =============================================================================
// No model, no agents: the propose/act callbacks are fakes. What is under test
// is the policy's validation (a rule that would act must say how it is undone),
// which rule an incident gets, what each level does and says, and the limits
// that hold whatever the policy says — a fix that has not held twice in a day
// is proposed, not tried a third time.

#include "heal.h"
#include "probes.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace funes::heal;
using funes::incidents::Incident;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const char* kPolicy = R"yaml(
rules:
  - when: "probe_services:bridge-funes"
    level: act
    action: reiniciar el bridge de WhatsApp de Funes
    agent: sysadmin
    tool: run_script
    args: {name: restart_unit, arguments: {unit: whatsapp-bridge-funes}}
    undo: none
  - when: "jobs:*"
    level: propose
    action: volver a correr el job
    agent: operator
    tool: run_job_now
    args: {id: "${match}"}
    undo: none
  - when: "probe_internet:internet"
    level: explain
    explain: "Necesita sudo: `nmcli con mod enp12s0 ipv4.never-default yes`."
)yaml";

Incident incident(const std::string& key, int64_t id = 1) {
    Incident i;
    i.id = id;
    i.key = key;
    i.summary = "something is down";
    return i;
}

struct Fixture {
    Policy policy;
    int64_t clock = 1'000'000;
    int proposals = 0, acts = 0;
    bool act_ok = true;
    std::string last_args;

    Fixture() {
        const std::string err = parse_policy(kPolicy, policy);
        if (!err.empty()) std::cerr << "policy: " << err << "\n";
    }
    Healer healer() {
        Options o;
        o.propose = [this](const Rule& r, const Incident&) {
            last_args = r.args.dump();
            return static_cast<int64_t>(40 + ++proposals);
        };
        o.act = [this](const Rule& r, const Incident&) {
            ++acts;
            last_args = r.args.dump();
            return std::make_pair(act_ok, std::string(act_ok ? "whatsapp-bridge-funes: active" : "exit 1: unit not found"));
        };
        o.now = [this] { return clock; };
        return Healer(policy, o);
    }
};

} // namespace

int test_policy_validation() {
    Policy p;
    CHECK(parse_policy(kPolicy, p).empty());
    CHECK(p.rules.size() == 3);
    CHECK(p.rules[0].level == Level::Act && p.rules[0].args["arguments"]["unit"] == "whatsapp-bridge-funes");

    Policy bad;
    // Acting without saying how it is undone.
    CHECK(!parse_policy("rules:\n  - when: a\n    level: act\n    action: x\n    agent: s\n    tool: t\n", bad).empty());
    // Only `none` until a runbook writes.
    CHECK(!parse_policy("rules:\n  - when: a\n    level: act\n    action: x\n    agent: s\n    tool: t\n    undo: snapshot\n", bad).empty());
    CHECK(!parse_policy("rules:\n  - when: a\n    level: propose\n    undo: none\n", bad).empty());
    CHECK(!parse_policy("rules:\n  - when: a\n    level: explain\n", bad).empty());
    CHECK(!parse_policy("rules:\n  - when: a\n    level: heroic\n    explain: x\n", bad).empty());
    CHECK(!parse_policy("rules: 3\n", bad).empty());
    CHECK(parse_policy("rules: []\n", bad).empty());
    return 0;
}

int test_match() {
    Fixture f;
    CHECK(match(f.policy, "probe_services:bridge-funes")->tool == "run_script");
    CHECK(match(f.policy, "jobs:3")->tool == "run_job_now");
    CHECK(match(f.policy, "probe_services:llm") == nullptr);
    CHECK(match(f.policy, "probe_services:bridge-funes-2") == nullptr);
    CHECK(match(f.policy, "jobs:(run)") == nullptr);
    return 0;
}

int test_each_level() {
    Fixture f;
    Healer h = f.healer();
    CHECK(h.on_open(incident("probe_services:llm")).empty());          // observe

    const std::string ex = h.on_open(incident("probe_internet:internet"));
    CHECK(ex.find("nmcli") != std::string::npos);
    CHECK(f.proposals == 0 && f.acts == 0);

    const std::string pr = h.on_open(incident("jobs:3"));
    CHECK(f.proposals == 1);
    CHECK(pr.find("aprobar 41") != std::string::npos);
    CHECK(pr.find("volver a correr el job") != std::string::npos);

    const std::string ac = h.on_open(incident("probe_services:bridge-funes"));
    CHECK(f.acts == 1);
    CHECK(ac.find("reiniciar el bridge") != std::string::npos);
    CHECK(f.last_args.find("whatsapp-bridge-funes") != std::string::npos);
    return 0;
}

// The job id in a jobs:* incident becomes the retry's argument.
int test_job_rule_gets_the_jobs_id() {
    Fixture f;
    Healer h = f.healer();
    h.on_open(incident("jobs:7"));
    CHECK(f.last_args.find("\"id\":7") != std::string::npos);
    return 0;
}

int test_act_twice_then_propose() {
    Fixture f;
    Healer h = f.healer();
    h.on_open(incident("probe_services:bridge-funes", 1));
    f.clock += 3600;
    h.on_open(incident("probe_services:bridge-funes", 2));
    f.clock += 3600;
    const std::string third = h.on_open(incident("probe_services:bridge-funes", 3));
    CHECK(f.acts == 2);
    CHECK(f.proposals == 1);
    CHECK(third.find("aprobar") != std::string::npos);
    // A day later it may act again.
    f.clock += 25 * 3600;
    h.on_open(incident("probe_services:bridge-funes", 4));
    CHECK(f.acts == 3);
    return 0;
}

int test_a_failed_act_says_so() {
    Fixture f;
    f.act_ok = false;
    Healer h = f.healer();
    const std::string t = h.on_open(incident("probe_services:bridge-funes"));
    CHECK(t.find("falló") != std::string::npos);
    CHECK(t.find("unit not found") != std::string::npos);
    return 0;
}

// Through the probe runner: the healer's text rides on the opening notice.
int test_the_notice_carries_the_action() {
    fs::path db = fs::temp_directory_path() / "funes_test_heal.db";
    fs::remove(db);
    funes::incidents::IncidentStore store(db.string());
    Fixture f;
    Healer h = f.healer();
    std::vector<std::string> sent;
    funes::probes::Options o;
    o.builtins.push_back({"jobs", 60, 1, [] {
        return std::vector<funes::incidents::CheckResult>{
            {"jobs:3", false, "warning", "Scheduled job #3 failed", "FAILED"}};
    }});
    o.on_open = [&h](const Incident& i) { return h.on_open(i); };
    o.notify = [&sent](const Incident& i) { sent.push_back(funes::probes::notice_text(i, "es")); return true; };
    funes::probes::Runner r(store, o);
    r.tick(1000);
    CHECK(sent.size() == 1);
    CHECK(sent[0].find("aprobar 41") != std::string::npos);
    CHECK(store.open_incidents()[0].action.find("aprobar 41") != std::string::npos);
    return 0;
}

// The shipped policy must load: a bad edit would leave every incident at
// "observe" without anyone noticing.
int test_shipped_policy(const char* path) {
    std::ifstream f(path);
    CHECK(f.good());
    std::stringstream s;
    s << f.rdbuf();
    Policy p;
    const std::string err = parse_policy(s.str(), p);
    if (!err.empty()) std::cerr << path << ": " << err << "\n";
    CHECK(err.empty());
    CHECK(match(p, "probe_services:bridge-funes") && match(p, "jobs:3"));
    return 0;
}

int main(int argc, char** argv) {
    int rc = 0;
    if (argc > 1) rc |= test_shipped_policy(argv[1]);
    rc |= test_policy_validation();
    rc |= test_match();
    rc |= test_each_level();
    rc |= test_job_rule_gets_the_jobs_id();
    rc |= test_act_twice_then_propose();
    rc |= test_a_failed_act_says_so();
    rc |= test_the_notice_carries_the_action();
    if (rc == 0) std::cout << "test_heal: all passed\n";
    return rc;
}
