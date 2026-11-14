// =============================================================================
// tests/test_probes.cpp — the probe runner (self-healing H0)
// =============================================================================
// Scripts are faked: what is under test is scheduling, the checks contract,
// turning results into incidents and incidents into notices — and that a
// probe that cannot run is reported, not mistaken for "all fine".

#include "probes.h"
#include "script_library.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace funes::probes;
using nlohmann::json;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

std::string temp_db(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("funes_test_probes_") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

struct Fixture {
    funes::incidents::IncidentStore store;
    json services = {{"checks", json::array({
        {{"name", "llm"}, {"ok", true}, {"summary", "llama-server :8080 answers"}},
        {{"name", "bridge"}, {"ok", true}, {"summary", "bridge :8091 answers"}}})}};
    std::string run_error;
    int runs = 0;
    std::vector<std::string> sent;
    bool bridge_up = true;
    int hour = 12;

    explicit Fixture(const char* name) : store(temp_db(name)) {}

    Options options() {
        Options o;
        o.list_scripts = [] { return std::vector<ScriptProbe>{{"probe_services", 300, 1}}; };
        o.run_script = [this](const std::string&, json& out) {
            ++runs;
            if (!run_error.empty()) return run_error;
            out = services;
            return std::string();
        };
        o.notify = [this](const Incident& i) {
            if (!bridge_up) return false;
            sent.push_back(notice_text(i, "es"));
            return true;
        };
        o.quiet_hours = "23-8";
        o.local_hour = [this](int64_t) { return hour; };
        return o;
    }
};

} // namespace

int test_parse_checks() {
    std::vector<funes::incidents::CheckResult> out;
    CHECK(parse_checks("p", json::parse(R"({"checks":[{"name":"a","ok":false,"summary":"A down",
        "severity":"critical","evidence":"refused"}]})"), out).empty());
    CHECK(out.size() == 1 && out[0].key == "p:a" && !out[0].ok && out[0].severity == "critical");
    out.clear();
    CHECK(!parse_checks("p", json::array(), out).empty());
    CHECK(!parse_checks("p", json{{"checks", json::array({{{"name", "a"}}})}}, out).empty());
    CHECK(!parse_checks("p", json{{"checks", json::array({{{"name", "a b"}, {"ok", true},
        {"summary", "x"}}})}}, out).empty());
    CHECK(!parse_checks("p", json{{"checks", json::array({{{"name", "a"}, {"ok", false},
        {"summary", "x"}, {"severity", "apocalyptic"}}})}}, out).empty());
    return 0;
}

int test_quiet_hours() {
    CHECK(in_quiet_hours("23-8", 23));
    CHECK(in_quiet_hours("23-8", 3));
    CHECK(!in_quiet_hours("23-8", 8));
    CHECK(!in_quiet_hours("23-8", 14));
    CHECK(in_quiet_hours("1-5", 2));
    CHECK(!in_quiet_hours("1-5", 6));
    CHECK(!in_quiet_hours("", 3));
    CHECK(!in_quiet_hours("nonsense", 3));
    return 0;
}

int test_failure_opens_notifies_and_resolves() {
    Fixture f("cycle");
    Runner r(f.store, f.options());
    auto rep = r.tick(1000);
    CHECK(rep.ran == 1 && rep.opened == 0 && f.sent.empty());

    f.services["checks"][0] = {{"name", "llm"}, {"ok", false},
                               {"summary", "llama-server :8080 does not answer"}};
    rep = r.tick(1100);                 // not due yet (every 300)
    CHECK(rep.ran == 0);
    rep = r.tick(1300);
    CHECK(rep.opened == 1 && rep.noticed == 1);
    CHECK(f.sent.size() == 1);
    CHECK(f.sent[0].find(":8080") != std::string::npos);

    rep = r.tick(1600);                 // still failing: no second message
    CHECK(rep.opened == 0 && f.sent.size() == 1);

    f.services["checks"][0]["ok"] = true;
    rep = r.tick(1900);
    CHECK(rep.resolved == 1 && f.sent.size() == 2);
    CHECK(f.sent[1].find("Resuelto") != std::string::npos);
    return 0;
}

// A probe that cannot run is an incident of its own, and running again
// closes it.
int test_a_probe_that_cannot_run_is_reported() {
    Fixture f("broken");
    f.run_error = "exit 1: Traceback …";
    Runner r(f.store, f.options());
    r.tick(1000);
    auto open = f.store.open_incidents();
    CHECK(open.size() == 1);
    CHECK(open[0].key == "probe_services:(run)");
    CHECK(f.sent.size() == 1);

    f.run_error.clear();
    r.tick(1300);
    CHECK(f.store.open_incidents().empty());
    return 0;
}

// Quiet hours hold a warning until morning; a bridge that is down holds it
// until it is back. Neither loses it.
int test_notices_wait_for_morning_and_for_the_bridge() {
    Fixture f("quiet");
    f.hour = 3;
    f.services["checks"][1]["ok"] = false;
    Runner r(f.store, f.options());
    r.tick(1000);
    CHECK(f.store.open_incidents().size() == 1 && f.sent.empty());

    f.hour = 9;
    f.bridge_up = false;
    r.tick(1300);
    CHECK(f.sent.empty());
    f.bridge_up = true;
    r.tick(1600);                       // within the retry backoff
    CHECK(f.sent.empty());
    r.tick(1300 + 16 * 60);
    CHECK(f.sent.size() == 1);
    return 0;
}

int test_critical_ignores_quiet_hours() {
    Fixture f("critical");
    f.hour = 3;
    f.services["checks"][0] = {{"name", "llm"}, {"ok", false}, {"severity", "critical"},
                               {"summary", "llama-server :8080 does not answer"}};
    Runner r(f.store, f.options());
    r.tick(1000);
    CHECK(f.sent.size() == 1);
    return 0;
}

int test_builtin_probe_and_unreported_checks() {
    Fixture f("builtin");
    std::vector<funes::incidents::CheckResult> jobs = {
        {"jobs:1", false, "warning", "Scheduled job #1 'Relvar' failed", "FAILED — …"},
        {"jobs:7", true, "", "ok", ""}};
    Options o = f.options();
    o.list_scripts = [] { return std::vector<ScriptProbe>{}; };
    o.builtins.push_back({"jobs", 120, 1, [&jobs] { return jobs; }});
    Runner r(f.store, o);
    r.tick(1000);
    CHECK(f.store.open_incidents().size() == 1);
    jobs = {{"jobs:7", true, "", "ok", ""}};   // job 1 was deleted
    r.tick(1200);
    CHECK(f.store.open_incidents().empty());
    return 0;
}

// The manifest side: a `probe:` block needs JSON output.
int test_probe_manifest() {
    fs::path dir = fs::temp_directory_path() / "funes_test_probe_lib";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "probe_x.py") << "print('{}')\n";
    std::ofstream(dir / "probe_x.yaml") <<
        "description: x\nrun: probe_x.py\ninterpreter: python3\neffects: none\n"
        "output: {format: json}\nprobe: {every: 600, fail_after: 3}\n";
    std::ofstream(dir / "probe_bad.py") << "print('x')\n";
    std::ofstream(dir / "probe_bad.yaml") <<
        "description: x\nrun: probe_bad.py\ninterpreter: python3\nprobe: {every: 600}\n";
    funes::ScriptSpec spec;
    CHECK(funes::load_script(dir.string(), "probe_x", spec).empty());
    CHECK(spec.probe_every == 600 && spec.probe_fail_after == 3);
    funes::ScriptSpec bad;
    CHECK(!funes::load_script(dir.string(), "probe_bad", bad).empty());
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_parse_checks();
    rc |= test_quiet_hours();
    rc |= test_failure_opens_notifies_and_resolves();
    rc |= test_a_probe_that_cannot_run_is_reported();
    rc |= test_notices_wait_for_morning_and_for_the_bridge();
    rc |= test_critical_ignores_quiet_hours();
    rc |= test_builtin_probe_and_unreported_checks();
    rc |= test_probe_manifest();
    if (rc == 0) std::cout << "test_probes: all passed\n";
    return rc;
}
