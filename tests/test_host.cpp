// =============================================================================
// tests/test_host.cpp — the machine as a memory owner (OS plan, phase 4)
// =============================================================================
// Parsers against real line shapes from yoda's journal and dpkg.log, and the
// collector against a fixture /proc + /sys + pid dir with the journal, the
// memory store and the links injected — so each rule is checked by what it
// does to memories, not by reading the code back.

#include "host_collector.h"
#include "host_events.h"
#include "host_sources.h"
#include "permissions.h"
#include "users.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace fs = std::filesystem;
using namespace funes::host;
using funes::Permissions;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const fs::path kBase = fs::temp_directory_path() / "funes_host_test";

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

std::string fresh_db(const char* name) {
    const fs::path p = kBase / (std::string(name) + ".db");
    for (const char* sfx : {"", "-wal", "-shm"}) fs::remove(p.string() + sfx);
    fs::create_directories(kBase);
    return p.string();
}

// One journal entry as `journalctl -o json` prints it.
std::string jline(const std::string& cursor, int64_t ts, const std::string& fields) {
    return "{\"__CURSOR\":\"" + cursor + "\",\"__REALTIME_TIMESTAMP\":\"" +
           std::to_string(ts) + "000000\"," + fields + "}\n";
}

int test_parse_journal() {
    const std::string out =
        jline("c1", 1000, R"("MESSAGE_ID":"39f53479d3a045ac8e11786248231fbf","PRIORITY":"6","USER_UNIT":"funes.service","MESSAGE":"Started")") +
        jline("c2", 1001, R"("MESSAGE_ID":"641257651c1b4ec9a8624d7a40a9e1e7","PRIORITY":"3","UNIT":"dream-mdns.service","EXECUTABLE":"/usr/bin/python3","ERRNO":"2","MESSAGE":"x")") +
        jline("c3", 1002, R"("PRIORITY":"3","UNIT":"dream-mdns.service","MESSAGE":"Changing to the requested working directory failed: \"/home/x\" pid 4411")") +
        jline("c4", 1003, R"("MESSAGE_ID":"fe6faa94e7774663a0da52717891d8ef","PRIORITY":"5","USER_UNIT":"run-p256679-i4450741.scope")") +
        jline("c5", 1004, R"("MESSAGE_ID":"ae8f7b866b0347b9af31fe1c80b127c0","PRIORITY":"6","USER_UNIT":"funes.service","CPU_USAGE_NSEC":"2283000000","MEMORY_PEAK":"215744512")") +
        jline("c6", 1005, R"("PRIORITY":"6","USER_UNIT":"yoda-monitor.service","MESSAGE":"GET /api/metrics 200")") +
        "-- cursor: ignored\nnot json\n";
    const JournalBatch b = parse_journal(out);
    CHECK(b.cursor == "c6");
    CHECK(b.events.size() == 5);                       // the access-log line is not an event
    CHECK(b.events[0].kind == "info" && b.events[0].subject == "funes.service");
    CHECK(b.events[1].kind == "failure");
    CHECK(b.events[1].detail == "could not execute \"/usr/bin/python3\" (errno 2)");
    CHECK(b.events[2].tainted);
    // Digits folded so the same failure with a new pid is the same row.
    CHECK(b.events[2].detail.find("pid #") != std::string::npos);
    CHECK(b.events[2].detail.find('"', 8) != std::string::npos);   // quoted
    CHECK(b.events[3].subject == "run-*.scope");
    CHECK(b.events[4].source == "unit-run" && b.events[4].kind == "sample");
    CHECK(b.events[4].value > 2.28 && b.events[4].value < 2.29);
    std::cout << "  ok: journal entries become field-built events, noise dropped\n";
    return 0;
}

int test_journal_argv_filters_at_source() {
    const auto a = journal_argv("");
    CHECK(std::find(a.begin(), a.end(), "--since=-10min") != a.end());
    CHECK(std::find(a.begin(), a.end(), "+") != a.end());
    CHECK(std::find(a.begin(), a.end(), "PRIORITY=3") != a.end());
    const auto b = journal_argv("s=abc");
    CHECK(std::find(b.begin(), b.end(), "--after-cursor=s=abc") != b.end());
    std::cout << "  ok: journalctl is asked for lifecycle + errors only\n";
    return 0;
}

int test_parse_dpkg_and_proc() {
    const auto ev = parse_dpkg(
        "2026-11-10 09:20:40 startup archives unpack\n"
        "2026-11-10 09:20:41 upgrade tailscale:amd64 1.104.4 1.104.5\n"
        "2026-11-10 09:20:42 status half-configured tailscale:amd64 1.104.5\n"
        "2026-11-10 09:21:00 install htop:amd64 <none> 3.3.0-4\n"
        "2026-11-10 09:22:00 remove nano:amd64 7.2-1 <none>\n");
    CHECK(ev.size() == 3);
    CHECK(ev[0].subject == "tailscale" && ev[0].detail == "upgraded 1.104.4 → 1.104.5");
    CHECK(ev[1].detail == "installed 3.3.0-4");
    CHECK(ev[2].detail == "removed 7.2-1");

    CHECK(psi_some_avg60("some avg10=1.00 avg60=12.50 avg300=3.00 total=9\nfull avg10=0 avg60=0 avg300=0 total=0\n") == 12.5);
    CHECK(!psi_some_avg60("garbage"));
    CHECK(vmstat_oom_kills("pgfault 1\noom_kill 16\n") == 16);
    CHECK(quote_log_text("a\"b\nc 123") == "\"a'b c #\"");
    std::cout << "  ok: dpkg actions, PSI, vmstat, quoting\n";
    return 0;
}

int test_run_subject() {
    CHECK(Collector::run_subject({"python3", "/srv/scriptlib/news_headlines.py", "--n=5"}) == "news_headlines.py");
    CHECK(Collector::run_subject({"/bin/sh", "-c", "ls"}) == "sh");
    CHECK(Collector::run_subject({"/usr/bin/env", "python3", "x.py"}) == "x.py");
    CHECK(Collector::run_subject({"pdftotext", "-layout", "a.pdf"}) == "pdftotext");
    std::cout << "  ok: runs are filed under the script, not the interpreter\n";
    return 0;
}

// ── the collector, against a fixture machine ─────────────────────────────────

struct Fixture {
    fs::path root = kBase / "machine";
    fs::path proc = root / "proc";
    fs::path sys  = root / "sys";
    fs::path pids = root / "pids";
    fs::path etc  = root / "etc";
    fs::path dpkg = root / "dpkg.log";

    std::map<int64_t, std::string> memories;          // id → text
    std::map<int64_t, bool>        tainted;
    std::vector<std::pair<int64_t, int64_t>> links;
    std::string journal_out;

    Fixture() {
        fs::remove_all(root);
        write(proc / "sys/kernel/osrelease", "6.12.96\n");
        write(proc / "sys/kernel/random/boot_id", "boot-1\n");
        write(proc / "vmstat", "oom_kill 16\n");
        write(proc / "pressure/cpu", "some avg10=0 avg60=1.00 avg300=0 total=0\n");
        write(proc / "pressure/memory", "some avg10=0 avg60=0.00 avg300=0 total=0\n");
        write(proc / "pressure/io", "some avg10=0 avg60=0.50 avg300=0 total=0\n");
        write(sys / "class/hwmon/hwmon1/name", "k10temp\n");
        write(sys / "class/hwmon/hwmon1/temp1_input", "55000\n");
        write(sys / "class/hwmon/hwmon1/temp1_label", "Tctl\n");
        write(pids / "llama_embed.pid", "4242\n");
        std::string cmd = std::string("llama-server\0-m\0/models/nomic.gguf\0", 36);
        write(proc / "4242/cmdline", cmd);
        write(etc / "launch_llama.sh", "v1\n");
        write(dpkg, "2026-11-01 10:00:00 upgrade old:amd64 1 2\n");
    }

    CollectorOptions options() const {
        CollectorOptions o;
        o.proc_root = proc.string();
        o.sys_root = sys.string();
        o.dpkg_log = dpkg.string();
        o.llama_pid_dir = pids.string();
        o.watch = {(etc / "launch_llama.sh").string()};
        o.disks = {"/"};
        o.funes_version = "7.6.0";
        return o;
    }

    Collector make(EventStore& store, CollectorOptions o) {
        return Collector(store, std::move(o),
            [this](const std::vector<std::string>&) -> std::optional<std::string> {
                std::string out = journal_out;
                journal_out.clear();
                return out;
            },
            [this](const std::string& text, bool t, const std::string&) -> int64_t {
                const int64_t id = static_cast<int64_t>(memories.size()) + 1;
                memories[id] = text;
                tainted[id] = t;
                return id;
            },
            [this](int64_t a, int64_t b) { links.emplace_back(a, b); });
    }

    bool remembered(const std::string& needle) const {
        for (const auto& [id, t] : memories)
            if (t.find(needle) != std::string::npos) return true;
        return false;
    }
    int64_t id_of(const std::string& needle) const {
        for (const auto& [id, t] : memories)
            if (t.find(needle) != std::string::npos) return id;
        return 0;
    }
};

int test_first_tick_announces_nothing() {
    Fixture f;
    EventStore store(fresh_db("first"));
    Collector c = f.make(store, f.options());
    c.tick(1'000'000);
    // What is already on the box is the starting point, not news.
    CHECK(f.memories.empty());
    CHECK(store.state("kernel") == "6.12.96");
    std::cout << "  ok: the first tick records the box without remembering it\n";
    return 0;
}

int test_changes_are_remembered() {
    Fixture f;
    EventStore store(fresh_db("changes"));
    Collector c = f.make(store, f.options());
    c.tick(1'000'000);

    write(f.proc / "sys/kernel/osrelease", "6.12.97\n");
    write(f.proc / "sys/kernel/random/boot_id", "boot-2\n");
    std::ofstream(f.dpkg, std::ios::app) << "2026-11-10 09:20:41 upgrade tailscale:amd64 1.104.4 1.104.5\n";
    write(f.etc / "launch_llama.sh", "v2 is longer\n");
    std::string cmd = std::string("llama-server\0-m\0/models/qwen27b.gguf\0", 38);
    write(f.pids / "llama_embed.pid", "5151\n");
    write(f.proc / "5151/cmdline", cmd);
    c.tick(1'000'060);

    CHECK(f.remembered("kernel: 6.12.96 → 6.12.97"));
    CHECK(f.remembered("booted (kernel 6.12.97)"));
    CHECK(f.remembered("package tailscale upgraded 1.104.4 → 1.104.5"));
    CHECK(!f.remembered("package old"));                       // history before first sight
    CHECK(f.remembered("launch_llama.sh was edited"));
    CHECK(f.remembered("llama-server 'embed' model changed: nomic.gguf → qwen27b.gguf"));
    for (const auto& [id, t] : f.tainted) CHECK(!t);           // all built from fields
    std::cout << "  ok: kernel, boot, dpkg, file edits and model swaps become memories\n";
    return 0;
}

int test_failure_folds_and_links_to_the_change_before_it() {
    Fixture f;
    EventStore store(fresh_db("fold"));
    Collector c = f.make(store, f.options());
    c.tick(1'000'000);

    write(f.etc / "launch_llama.sh", "edited\n");
    c.tick(1'000'060);
    const int64_t change = f.id_of("launch_llama.sh was edited");
    CHECK(change > 0);

    // A unit in a restart loop: the same error every few seconds.
    for (int i = 0; i < 3; ++i) {
        f.journal_out = jline("k" + std::to_string(i), 1'000'100 + i * 20,
            R"("PRIORITY":"3","UNIT":"dream-mdns.service","MESSAGE":"chdir failed, pid )" +
            std::to_string(900 + i) + "\"");
        c.tick(1'000'100 + i * 60);
    }
    size_t failures = 0;
    for (const auto& [id, t] : f.memories) failures += t.find("dream-mdns") != std::string::npos;
    CHECK(failures == 1);                                      // folded: one row, one memory
    const int64_t failure = f.id_of("dream-mdns");
    CHECK(f.tainted[failure]);                                 // it quotes a log line
    CHECK(std::find(f.links.begin(), f.links.end(), std::make_pair(failure, change)) != f.links.end());
    const auto rows = store.query(0, {"failure"}, "dream", 10);
    CHECK(rows.size() == 1 && rows[0].count == 3);

    // Still failing a day later: a new row, and a new memory.
    f.journal_out = jline("k9", 1'000'100 + 90'000,
        R"("PRIORITY":"3","UNIT":"dream-mdns.service","MESSAGE":"chdir failed, pid 1")");
    c.tick(1'000'100 + 90'000);
    failures = 0;
    for (const auto& [id, t] : f.memories) failures += t.find("dream-mdns") != std::string::npos;
    CHECK(failures == 2);
    CHECK(store.state("journal.cursor") == "k9");
    std::cout << "  ok: a restart loop is one memory a day, linked to the change before it\n";
    return 0;
}

int test_oom_and_dead_llama_are_failures() {
    Fixture f;
    EventStore store(fresh_db("oom"));
    Collector c = f.make(store, f.options());
    c.tick(1'000'000);
    write(f.proc / "vmstat", "oom_kill 18\n");
    fs::remove_all(f.proc / "4242");                           // pid file stays, process gone
    c.tick(1'000'060);
    CHECK(f.remembered("kernel: the OOM killer killed 2 process(es)"));
    // The cgroup test's deliberate OOM, on every deploy: not news.
    f.journal_out = jline("t1", 1'000'090, R"("MESSAGE_ID":"fe6faa94e7774663a0da52717891d8ef","PRIORITY":"5","USER_UNIT":"funes-test-cgroup.scope")");
    write(f.proc / "vmstat", "oom_kill 19\n");
    const size_t before = f.memories.size();
    c.tick(1'000'090);
    CHECK(f.memories.size() == before);
    CHECK(f.remembered("llama-server 'embed' died (pid 4242 is gone"));
    fs::remove(f.pids / "llama_embed.pid");
    c.tick(1'000'120);
    CHECK(f.remembered("llama-server 'embed' stopped"));
    std::cout << "  ok: OOM kills and a dead llama-server are remembered as failures\n";
    return 0;
}

int test_run_baseline() {
    Fixture f;
    EventStore store(fresh_db("runs"));
    Collector c = f.make(store, f.options());
    funes::cgroup::Usage u;
    u.memory_peak = 50 << 20;
    int64_t t = 2'000'000;
    for (int i = 0; i < 6; ++i) {
        u.cpu_usec = 3'000'000 + i * 100'000;                  // ~3 s each
        c.record_run({"python3", "/lib/backup.py"}, u, t += 60);
        c.tick(t);
    }
    CHECK(f.memories.empty());
    u.cpu_usec = 40'000'000;                                   // 40 s
    c.record_run({"python3", "/lib/backup.py"}, u, t += 60);
    c.tick(t);
    CHECK(f.remembered("backup.py used 40.0s of CPU; its median over the last 6 runs is 3.2s"));
    // The collector's own journalctl is not a series.
    c.record_run({"journalctl", "--user"}, u, t += 60);
    c.tick(t);
    CHECK(store.values("run", "journalctl", 0).empty());
    std::cout << "  ok: a run far above its own median is an anomaly\n";
    return 0;
}

int test_daily_ceiling() {
    Fixture f;
    EventStore store(fresh_db("cap"));
    CollectorOptions o = f.options();
    o.promotions_per_day = 2;
    Collector c = f.make(store, o);
    c.tick(1'000'000);
    std::ofstream(f.dpkg, std::ios::app)
        << "2026-11-10 09:20:41 upgrade a:amd64 1 2\n2026-11-10 09:20:41 upgrade b:amd64 1 2\n"
        << "2026-11-10 09:20:41 upgrade c:amd64 1 2\n2026-11-10 09:20:41 upgrade d:amd64 1 2\n";
    c.tick(1'000'060);
    CHECK(f.memories.size() == 2);
    CHECK(store.query(0, {"change"}, "", 10).size() == 4);      // still recorded
    std::cout << "  ok: the daily ceiling stops memories, not events\n";
    return 0;
}

// ── the account ──────────────────────────────────────────────────────────────

int test_system_account_cannot_be_used() {
    UserStore users(fresh_db("users"));
    // Not before a person: #1 is who pre-4.0 migrations hand legacy data to.
    CHECK(users.ensure_system_account("machine") == 0);
    const int64_t person1 = users.create_user("julio", "pw-pw-pw-pw", "", UserStore::ROLE_ADMIN);
    CHECK(person1 == 1);
    const int64_t uid = users.ensure_system_account("machine");
    CHECK(uid > 1);
    CHECK(users.ensure_system_account("machine") == uid);       // idempotent
    CHECK(!users.find_by_username("machine"));
    CHECK(!users.find_by_id(uid));
    CHECK(!users.verify_login("machine", "!"));
    CHECK(!users.verify_login("machine", ""));
    CHECK(users.count() == 1);                                 // people only
    CHECK(users.list_users().size() == 1);
    CHECK(!users.set_password(uid, "now-i-can-log-in"));
    CHECK(!users.verify_login("machine", "now-i-can-log-in"));
    CHECK(!users.delete_user(uid));

    // A person already called `machine` keeps their account.
    UserStore other(fresh_db("users2"));
    const int64_t person = other.create_user("machine", "pw-pw-pw-pw", "", UserStore::ROLE_MEMBER);
    CHECK(person > 0);
    CHECK(other.ensure_system_account("machine") == 0);
    CHECK(other.find_by_id(person)->role == UserStore::ROLE_MEMBER);

    // The tools that read it are an admin's unless granted.
    const Permissions member = Permissions::parse("{}", false);
    CHECK(!member.allows_tool("host_changes"));
    CHECK(!member.allows_tool("machine_recall"));
    CHECK(Permissions::parse("{\"tools\":{\"host_changes\":true}}", false).allows_tool("host_changes"));
    CHECK(Permissions::parse("{}", true).allows_tool("machine_recall"));
    std::cout << "  ok: the machine account cannot log in, be listed, edited or deleted\n";
    return 0;
}

} // namespace

int main() {
    int failed = 0;
    failed += test_parse_journal();
    failed += test_journal_argv_filters_at_source();
    failed += test_parse_dpkg_and_proc();
    failed += test_run_subject();
    failed += test_first_tick_announces_nothing();
    failed += test_changes_are_remembered();
    failed += test_failure_folds_and_links_to_the_change_before_it();
    failed += test_oom_and_dead_llama_are_failures();
    failed += test_run_baseline();
    failed += test_daily_ceiling();
    failed += test_system_account_cannot_be_used();
    fs::remove_all(kBase);
    if (failed) { std::cerr << failed << " test(s) failed\n"; return 1; }
    std::cout << "host: all tests passed\n";
    return 0;
}
