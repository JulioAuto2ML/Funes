// =============================================================================
// src/core/host_collector.h — the machine as a memory owner (OS plan, phase 4)
// =============================================================================
// A background pass that turns what yoda does into two things:
//
//   - events (host_events.h): everything the sources report, folded and kept
//     for a retention window — what `host_changes` reads, no model involved;
//   - memories, in the `machine` system account's pool: only *changes* (a
//     package upgraded, a unit file edited, a kernel, a model swapped in
//     llama-server, Funes deployed) and *failures/anomalies* (a unit failed,
//     the OOM killer fired, a script took four times its median CPU). Each is
//     embedded like any memory — yoda's embedding server is otherwise idle —
//     and a failure is linked to the changes just before it, so "what changed
//     before it broke" is a recall away.
//
// Three rules, each from something this repo has met:
//   - Events are not memories. A unit in a restart loop logs ~16k lines a day
//     on yoda; it becomes one row a day and one memory a day.
//   - Baselines are arithmetic in C++ (median over the series), never the
//     model's.
//   - Log text is hostile: quoted, capped, tainted (host_sources.h).
//
// Nothing here calls the model. What it needs from the rest of Funes —
// running journalctl, storing a memory, linking two — is injected, so the
// tests drive it with fixtures and the collector cannot reach anything else.
// =============================================================================

#pragma once
#include "cgroup.h"
#include "host_events.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace funes::host {

struct CollectorOptions {
    std::string proc_root     = "/proc";
    std::string sys_root      = "/sys";
    std::string dpkg_log      = "/var/log/dpkg.log";
    std::string llama_pid_dir = "/tmp/llama_pids";
    std::vector<std::string> disks;   // mount points to sample
    std::vector<std::string> watch;   // files / directories whose edits are changes
    std::string funes_version;
    bool        journal = true;

    int sample_every_s     = 300;   // PSI, temperatures, VRAM, disks
    int promotions_per_day = 200;   // FUNES_MACHINE_PROMOTIONS_PER_DAY: a runaway guard
    int retention_days     = 30;    // samples; other events are kept 4× as long
};

// Runs argv (journalctl) and returns its stdout, or nullopt if it failed.
using CommandRunner = std::function<std::optional<std::string>(const std::vector<std::string>&)>;
// Stores one machine memory; returns its id (0 = not stored).
using RememberFn = std::function<int64_t(const std::string& text, bool tainted,
                                         const std::string& origin)>;
// Links a failure's memory to a change's memory that preceded it.
using LinkFn = std::function<void(int64_t from_memory, int64_t to_memory)>;

class Collector {
public:
    Collector(EventStore& store, CollectorOptions opt, CommandRunner run,
              RememberFn remember, LinkFn link);

    struct Report {
        int events   = 0;   // rows added or folded
        int promoted = 0;   // memories written
    };
    Report tick(int64_t now);

    // A child process finished in a cgroup of its own (process_runner's usage
    // observer). Queued; the next tick turns it into a sample and checks it
    // against that program's baseline. Thread-safe.
    void record_run(const std::vector<std::string>& argv, const funes::cgroup::Usage& usage,
                    int64_t now);

    // The series name a command line is filed under: the script, not the
    // interpreter ("python3 /…/news_headlines.py" → "news_headlines.py").
    static std::string run_subject(const std::vector<std::string>& argv);

    // "[2026-11-11 05:58] package tailscale upgraded 1.104.4 → 1.104.5" — the
    // text a promoted event is remembered as. Local time.
    static std::string memory_text(const Event& e);

private:
    struct Run {
        std::string subject;
        double      cpu_s = 0;
        double      mem_mb = 0;
        bool        oom = false;
        int64_t     ts = 0;
    };

    void add(Event e, int64_t fold_gap, int64_t fold_span, Report& r);
    void promote(int64_t id, const Event& e, Report& r);
    void check_baseline(const Event& sample, Report& r);

    void collect_journal(int64_t now, Report& r);
    void collect_dpkg(int64_t now, Report& r);
    void collect_boot(int64_t now, Report& r);
    void collect_oom(int64_t now, Report& r);
    void collect_llama(int64_t now, Report& r);
    void collect_files(int64_t now, Report& r);
    void collect_samples(int64_t now, Report& r);
    void collect_runs(Report& r);

    EventStore&      store_;
    CollectorOptions opt_;
    CommandRunner    run_;
    RememberFn       remember_;
    LinkFn           link_;

    std::mutex       runs_mu_;
    std::deque<Run>  runs_;
    bool             cap_logged_ = false;
    // OOM kills this tick inside funes-test-* scopes — the cgroup test causes
    // them on purpose, on every deploy — taken off the kernel's counter.
    int64_t          test_ooms_ = 0;
};

} // namespace funes::host
