// src/core/host_collector.cpp — see host_collector.h
#include "host_collector.h"
#include "host_sources.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace funes::host {

namespace {

constexpr int64_t HOUR = 3600;
constexpr int64_t DAY  = 24 * HOUR;

// How repeats fold (EventStore::add): a failure that keeps happening is one
// row while it keeps happening, and a new row — a new memory — once a day.
constexpr int64_t FAILURE_GAP = HOUR,     FAILURE_SPAN = DAY;
constexpr int64_t ANOMALY_GAP = 6 * HOUR, ANOMALY_SPAN = DAY;

// A run or a pressure reading is an anomaly against its own past when there
// are at least this many points to judge by, it is this many times the
// median, and it clears an absolute floor — four times nothing is nothing.
constexpr size_t BASELINE_MIN_POINTS = 5;
constexpr double BASELINE_FACTOR     = 4.0;
constexpr double RUN_CPU_FLOOR_S     = 10.0;

// Absolute floors for pressure (some avg60, percent): below these nobody
// waited long enough to care, whatever the baseline says.
const std::map<std::string, double> PSI_FLOOR = {{"cpu", 40}, {"memory", 10}, {"io", 25}};
constexpr double DISK_FULL_PCT   = 90;
constexpr double TEMP_NO_CRIT_C  = 90;   // when a sensor publishes no critical point
constexpr double TEMP_CRIT_MARGIN = 5;

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

std::string fmt(double v, int prec = 1) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(prec);
    o << v;
    return o.str();
}

std::string local_time(int64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string basename_of(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); } else cur += c;
    }
    out.push_back(cur);
    return out;
}

} // namespace

Collector::Collector(EventStore& store, CollectorOptions opt, CommandRunner run,
                     RememberFn remember, LinkFn link)
    : store_(store), opt_(std::move(opt)), run_(std::move(run)),
      remember_(std::move(remember)), link_(std::move(link)) {}

std::string Collector::run_subject(const std::vector<std::string>& argv) {
    if (argv.empty()) return "unknown";
    static const std::set<std::string> interpreters = {
        "python", "python3", "bash", "sh", "node", "perl", "ruby", "uv", "env"};
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string b = basename_of(argv[i]);
        if (i + 1 < argv.size() && interpreters.count(b) && !argv[i + 1].empty()
            && argv[i + 1][0] != '-')
            continue;
        if (i > 0 && !argv[i].empty() && argv[i][0] == '-') return basename_of(argv[0]);
        return b;
    }
    return basename_of(argv.back());
}

std::string Collector::memory_text(const Event& e) {
    std::string what;
    if (e.source == "dpkg")       what = "package " + e.subject + " " + e.detail;
    else if (e.source == "llama") what = "llama-server '" + e.subject + "' " + e.detail;
    else if (e.source == "file")  what = e.subject + " was " + e.detail;
    else if (e.source == "journal" || e.source == "unit-run" || e.source == "run")
        what = e.subject + " " + e.detail;
    else                          what = e.subject + ": " + e.detail;
    return "[" + local_time(e.ts) + "] " + what;
}

void Collector::record_run(const std::vector<std::string>& argv,
                           const funes::cgroup::Usage& usage, int64_t now) {
    Run r;
    r.subject = run_subject(argv);
    // The collector's own journalctl every minute is not the machine's news.
    if (r.subject == "journalctl") return;
    r.cpu_s  = static_cast<double>(usage.cpu_usec) / 1e6;
    r.mem_mb = static_cast<double>(usage.memory_peak) / (1024.0 * 1024.0);
    r.oom    = usage.oom_killed;
    r.ts     = now;
    std::lock_guard<std::mutex> lock(runs_mu_);
    if (runs_.size() < 1000) runs_.push_back(std::move(r));
}

void Collector::add(Event e, int64_t fold_gap, int64_t fold_span, Report& r) {
    bool folded = false;
    const int64_t id = store_.add(e, fold_gap, fold_span, &folded);
    ++r.events;
    if (!folded && (e.kind == "change" || e.kind == "failure" || e.kind == "anomaly"))
        promote(id, e, r);
    if (e.kind == "sample" && (e.source == "run" || e.source == "unit-run"))
        check_baseline(e, r);
}

void Collector::promote(int64_t id, const Event& e, Report& r) {
    if (store_.promoted_since(e.ts - DAY) >= opt_.promotions_per_day) {
        if (!cap_logged_)
            std::cerr << "[machine] " << opt_.promotions_per_day << " memories in a day — "
                         "the ceiling (FUNES_MACHINE_PROMOTIONS_PER_DAY); events are still "
                         "recorded, host_changes still sees them\n";
        cap_logged_ = true;
        return;
    }
    cap_logged_ = false;
    const int64_t mem = remember_(memory_text(e), e.tainted, "host:" + e.source + ":" + e.subject);
    if (mem <= 0) return;
    store_.set_memory(id, mem);
    ++r.promoted;

    // What changed before it broke: link a failure or anomaly to every
    // remembered change in the two hours before it, and to changes to the
    // same subject in the day before that. The link judge (backfill_links)
    // can still add or contradict; these are the ones arithmetic can see.
    if (e.kind == "change") return;
    std::set<int64_t> linked;
    for (const auto& c : store_.query(e.ts - 2 * HOUR, {"change"}, "", 50))
        if (c.memory_id > 0 && c.ts <= e.ts && linked.insert(c.memory_id).second)
            link_(mem, c.memory_id);
    for (const auto& c : store_.query(e.ts - DAY, {"change"}, e.subject, 50))
        if (c.memory_id > 0 && c.ts <= e.ts && c.subject == e.subject
            && linked.insert(c.memory_id).second)
            link_(mem, c.memory_id);
}

void Collector::check_baseline(const Event& sample, Report& r) {
    // The series without the point being judged (it is the newest).
    std::vector<double> past = store_.values(sample.source, sample.subject, sample.ts - 30 * DAY, 51);
    if (!past.empty()) past.erase(past.begin());
    if (past.size() < BASELINE_MIN_POINTS) return;
    const double med = median(past);
    if (sample.value < RUN_CPU_FLOOR_S || sample.value < BASELINE_FACTOR * std::max(med, 0.1))
        return;
    Event a;
    a.ts      = sample.ts;
    a.source  = sample.source;
    a.kind    = "anomaly";
    a.subject = sample.subject;
    a.value   = sample.value;
    a.detail  = "used " + fmt(sample.value) + "s of CPU; its median over the last " +
                std::to_string(past.size()) + " runs is " + fmt(med) + "s";
    add(a, ANOMALY_GAP, ANOMALY_SPAN, r);
}

Collector::Report Collector::tick(int64_t now) {
    Report r;
    // Each source on its own: one that throws (a file gone mid-read) must not
    // stop the others.
    const std::pair<const char*, void (Collector::*)(int64_t, Report&)> sources[] = {
        {"journal", &Collector::collect_journal}, {"dpkg", &Collector::collect_dpkg},
        {"boot", &Collector::collect_boot},       {"oom", &Collector::collect_oom},
        {"llama", &Collector::collect_llama},     {"files", &Collector::collect_files},
        {"samples", &Collector::collect_samples},
    };
    for (const auto& [name, fn] : sources) {
        try { (this->*fn)(now, r); }
        catch (const std::exception& e) {
            std::cerr << "[machine] " << name << " source failed: " << e.what() << "\n";
        }
    }
    try { collect_runs(r); }
    catch (const std::exception& e) { std::cerr << "[machine] runs failed: " << e.what() << "\n"; }

    const auto last = store_.state("prune.last");
    if (!last || now - std::stoll(*last) > DAY) {
        store_.prune(now - opt_.retention_days * DAY, now - 4LL * opt_.retention_days * DAY);
        store_.set_state("prune.last", std::to_string(now));
    }
    return r;
}

// ── sources ──────────────────────────────────────────────────────────────────

void Collector::collect_journal(int64_t, Report& r) {
    if (!opt_.journal || !run_) return;
    const std::string cursor = store_.state("journal.cursor").value_or("");
    const auto out = run_(journal_argv(cursor));
    if (!out) return;
    JournalBatch b = parse_journal(*out);
    for (auto& e : b.events) {
        // Funes's own test scopes (tests/CMakeLists.txt names them): their
        // failures are the test working.
        if (e.subject.rfind("funes-test-", 0) == 0) {
            if (e.detail.find("OOM killer") != std::string::npos) ++test_ooms_;
            continue;
        }
        if (e.kind == "sample") add(std::move(e), 0, 0, r);
        else                    add(std::move(e), FAILURE_GAP, FAILURE_SPAN, r);
    }
    if (!b.cursor.empty()) store_.set_state("journal.cursor", b.cursor);
}

void Collector::collect_dpkg(int64_t, Report& r) {
    struct stat st{};
    if (stat(opt_.dpkg_log.c_str(), &st) != 0) return;
    const std::string ino = std::to_string(st.st_ino);
    const auto known_ino = store_.state("dpkg.inode");
    int64_t offset = std::stoll(store_.state("dpkg.offset").value_or("-1"));
    // First sight: start at the end. The log's history is not news.
    if (!known_ino || offset < 0) offset = st.st_size;
    // Rotated or truncated: the new file is read from its start.
    else if (*known_ino != ino || offset > st.st_size) offset = 0;

    if (offset < st.st_size) {
        auto text = read_text(opt_.dpkg_log, 4 << 20);
        if (text && static_cast<int64_t>(text->size()) > offset) {
            std::string tail = text->substr(static_cast<size_t>(offset));
            // Only whole lines; a half-written one waits for the next tick.
            const size_t nl = tail.find_last_of('\n');
            tail = nl == std::string::npos ? "" : tail.substr(0, nl + 1);
            offset += static_cast<int64_t>(tail.size());
            for (auto& e : parse_dpkg(tail)) add(std::move(e), 0, 0, r);
        }
    }
    store_.set_state("dpkg.inode", ino);
    store_.set_state("dpkg.offset", std::to_string(offset));
}

void Collector::collect_boot(int64_t now, Report& r) {
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    };
    const std::string kernel = trim(read_text(opt_.proc_root + "/sys/kernel/osrelease", 256).value_or(""));
    const std::string boot   = trim(read_text(opt_.proc_root + "/sys/kernel/random/boot_id", 64).value_or(""));

    // Each is a change only against a value seen before: the collector's first
    // tick on a box records what is there, it does not announce it.
    auto changed = [this](const std::string& key, const std::string& now_value) {
        const auto before = store_.state(key);
        if (now_value.empty()) return std::optional<std::string>{};
        if (!before || *before == now_value) {
            store_.set_state(key, now_value);
            return std::optional<std::string>{};
        }
        store_.set_state(key, now_value);
        return before;
    };
    if (changed("boot.id", boot)) {
        Event e{0, now, now, "boot", "change", "system", "booted (kernel " + kernel + ")"};
        add(e, 0, 0, r);
    }
    if (auto before = changed("kernel", kernel)) {
        Event e{0, now, now, "boot", "change", "kernel", *before + " → " + kernel};
        add(e, 0, 0, r);
    }
    if (auto before = changed("funes.version", opt_.funes_version)) {
        Event e{0, now, now, "funes", "change", "Funes", "deployed: " + *before + " → " + opt_.funes_version};
        add(e, 0, 0, r);
    }
}

void Collector::collect_oom(int64_t now, Report& r) {
    const auto text = read_text(opt_.proc_root + "/vmstat");
    const auto kills = text ? vmstat_oom_kills(*text) : std::nullopt;
    if (!kills) return;
    const auto before = store_.state("oom.kills");
    store_.set_state("oom.kills", std::to_string(*kills));
    if (!before) return;
    const int64_t delta = *kills - std::stoll(*before) - test_ooms_;
    test_ooms_ = 0;
    if (delta <= 0) return;   // a reboot resets the counter
    Event e{0, now, now, "oom", "failure", "kernel",
            "the OOM killer killed " + std::to_string(delta) + " process(es)"};
    e.count = delta;
    add(e, 0, 0, r);
}

void Collector::collect_llama(int64_t now, Report& r) {
    const auto current = read_llama(opt_.llama_pid_dir, opt_.proc_root);
    const auto names_state = store_.state("llama.names");
    std::set<std::string> before_names;
    if (names_state && !names_state->empty())
        for (const auto& n : split(*names_state, ',')) before_names.insert(n);
    const bool first = !names_state.has_value();

    std::string names;
    for (const auto& [name, st] : current) {
        names += (names.empty() ? "" : ",") + name;
        const auto parts = split(st, '|');   // pid|model|alive
        const std::string model = parts.size() > 1 ? basename_of(parts[1]) : "";
        const bool alive = parts.size() > 2 && parts[2] == "1";
        const auto prev = store_.state("llama." + name);
        store_.set_state("llama." + name, st);
        if (first || (prev && *prev == st)) continue;

        Event e{0, now, now, "llama", "change", name, ""};
        if (!prev) {
            e.detail = alive ? "started" + (model.empty() ? "" : " with " + model) : "pid file without a process";
        } else {
            const auto p = split(*prev, '|');
            const std::string pmodel = p.size() > 1 ? basename_of(p[1]) : "";
            const bool palive = p.size() > 2 && p[2] == "1";
            if (palive && !alive) {
                e.kind   = "failure";
                e.detail = "died (pid " + parts[0] + " is gone, its pid file is still there)";
            } else if (!model.empty() && !pmodel.empty() && model != pmodel) {
                e.detail = "model changed: " + pmodel + " → " + model;
            } else if (alive) {
                e.detail = "restarted" + (model.empty() ? "" : " with " + model);
            } else {
                continue;
            }
        }
        add(e, 0, 0, r);
    }
    if (!first)
        for (const auto& n : before_names)
            if (!current.count(n)) {
                Event e{0, now, now, "llama", "change", n, "stopped"};
                add(e, 0, 0, r);
            }
    store_.set_state("llama.names", names);
}

void Collector::collect_files(int64_t now, Report& r) {
    if (opt_.watch.empty()) return;
    const auto current = fingerprint(opt_.watch);
    const auto known_state = store_.state("files.known");
    std::set<std::string> known;
    if (known_state && !known_state->empty())
        for (const auto& p : split(*known_state, '\n')) known.insert(p);
    const bool first = !known_state.has_value();

    std::string all;
    for (const auto& [path, fp] : current) {
        all += (all.empty() ? "" : "\n") + path;
        const auto prev = store_.state("file:" + path);
        store_.set_state("file:" + path, fp);
        if (first || (prev && *prev == fp)) continue;
        Event e{0, now, now, "file", "change", path, prev ? "edited" : "created"};
        add(e, 0, 0, r);
    }
    if (!first)
        for (const auto& p : known)
            if (!current.count(p)) {
                Event e{0, now, now, "file", "change", p, "removed"};
                add(e, 0, 0, r);
            }
    store_.set_state("files.known", all);
}

void Collector::collect_samples(int64_t now, Report& r) {
    const auto last = store_.state("samples.last");
    if (last && now - std::stoll(*last) < opt_.sample_every_s) return;
    store_.set_state("samples.last", std::to_string(now));

    for (const auto& [res, floor] : PSI_FLOOR) {
        const auto text = read_text(opt_.proc_root + "/pressure/" + res);
        const auto v = text ? psi_some_avg60(*text) : std::nullopt;
        if (!v) continue;
        Event s{0, now, now, "psi", "sample", "pressure:" + res, "", *v};
        add(s, 0, 0, r);
        std::vector<double> past = store_.values("psi", s.subject, now - 7 * DAY, 300);
        if (!past.empty()) past.erase(past.begin());
        const double med = median(past);
        if (*v >= floor && (past.size() < BASELINE_MIN_POINTS || *v >= BASELINE_FACTOR * std::max(med, 1.0))) {
            Event a{0, now, now, "psi", "anomaly", s.subject,
                    res + " pressure at " + fmt(*v) + "% (some avg60; median " + fmt(med) + "%)", *v};
            add(a, ANOMALY_GAP, ANOMALY_SPAN, r);
        }
    }
    for (const auto& t : read_temperatures(opt_.sys_root)) {
        add({0, now, now, "temp", "sample", t.subject, "", t.value}, 0, 0, r);
        const double limit = t.limit > 0 ? t.limit - TEMP_CRIT_MARGIN : TEMP_NO_CRIT_C;
        if (t.value >= limit)
            add({0, now, now, "temp", "anomaly", t.subject,
                 "at " + fmt(t.value) + "°C (critical " + (t.limit > 0 ? fmt(t.limit) + "°C" : "unknown") + ")",
                 t.value}, ANOMALY_GAP, ANOMALY_SPAN, r);
    }
    for (const auto& g : read_vram(opt_.sys_root))
        add({0, now, now, "gpu", "sample", g.subject, "", g.value}, 0, 0, r);
    for (const auto& d : read_disks(opt_.disks)) {
        add({0, now, now, "disk", "sample", d.subject, "", d.value}, 0, 0, r);
        if (d.value >= DISK_FULL_PCT)
            add({0, now, now, "disk", "anomaly", d.subject, fmt(d.value) + "% full", d.value},
                ANOMALY_GAP, ANOMALY_SPAN, r);
    }
}

void Collector::collect_runs(Report& r) {
    std::deque<Run> runs;
    {
        std::lock_guard<std::mutex> lock(runs_mu_);
        runs.swap(runs_);
    }
    for (const auto& run : runs) {
        Event s{0, run.ts, run.ts, "run", "sample", run.subject,
                "cpu=" + fmt(run.cpu_s, 2) + "s mem=" + fmt(run.mem_mb) + "MB", run.cpu_s};
        add(s, 0, 0, r);
        if (run.oom)
            add({0, run.ts, run.ts, "run", "failure", run.subject,
                 "was OOM-killed in its cgroup (peak " + fmt(run.mem_mb) + "MB)"},
                FAILURE_GAP, FAILURE_SPAN, r);
    }
}

} // namespace funes::host
