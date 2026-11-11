// =============================================================================
// src/core/cgroup.h — one cgroup per child process (OS plan, phase 1)
// =============================================================================
// Every program Funes starts — a library script, an execute_shell command, a
// scheduled shell job, pdftotext — used to run in the server's own cgroup,
// limited by nothing but a timeout, and killed on timeout with kill(-pgid),
// which a child that calls setsid() or double-forks walks straight out of.
//
// With a delegated subtree (`Delegate=yes` on the unit, FUNES_CGROUPS=1) the
// server moves itself into a `server/` leaf and each child gets a sibling
// cgroup of its own:
//
//   …/funes.service/server/                 the server's threads
//   …/funes.service/exec-<n>-<program>/     one child and everything it starts
//
// That buys three things a process group cannot: limits the kernel enforces
// (memory.max, pids.max, cpu.weight), a kill nothing escapes (cgroup.kill),
// and accounting that belongs to one call by construction — peak memory, CPU
// time, an OOM kill — read off the cgroup before it is removed, and printed on
// the `[tool]` line of the call that caused it. The cgroup's name is the join
// key between what the model asked for and what the machine did.
//
// The child joins its cgroup itself, through the `funes _exec` trampoline
// (process_runner.h): the parent's fork child still does nothing but exec,
// and the trampoline is a fresh process image that writes to cgroup.procs
// before exec'ing the real program. Phase 2 (Landlock, seccomp) hangs off the
// same trampoline.
//
// Off, everything runs as it did before: no cgroup, no limits, no stats, and
// kill(-pgid) on timeout. Funes runs unprivileged, so nothing here needs root —
// only a cgroup systemd has handed over.

#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace funes::cgroup {

// Zero means "not set" — the child inherits whatever the parent has.
struct Limits {
    int64_t memory_max_bytes = 0;  // memory.max (and memory.swap.max = 0, or a
                                   // child under its limit swaps instead of
                                   // being stopped)
    int64_t pids_max         = 0;  // pids.max: processes + threads
    int     cpu_weight       = 0;  // cpu.weight, 1..10000; the server's is 100
};

// FUNES_CHILD_MEMORY_MB (4096), FUNES_CHILD_PIDS_MAX (512),
// FUNES_CHILD_CPU_WEIGHT (50: a tool call yields the CPU to the server
// answering chat). 0 or a negative value turns that limit off.
Limits default_limits();

// What one child cost, read just before its cgroup was removed. A counter the
// kernel does not offer (memory.peak before 5.19, pids.peak before 6.1, or a
// controller not delegated) stays 0.
struct Usage {
    std::string name;              // exec-<n>-<program>
    uint64_t    cpu_usec      = 0; // cpu.stat usage_usec
    uint64_t    memory_peak   = 0; // bytes
    uint64_t    pids_peak     = 0;
    bool        oom_killed    = false;  // memory.events oom_kill > 0
    bool        killed_leftovers = false;  // something was still running after
                                           // the main child exited (a daemon, a
                                           // setsid'd grandchild) and was killed
};

// " {exec-12-sh cpu=0.31s mem=12.4MB pids=3}" — with " oom" or " leftovers
// killed" inside the braces when they happened. Leading space, no newline.
std::string format_usage(const Usage& u);

struct Status {
    bool        enabled = false;
    std::string dir;          // the delegated cgroup (…/funes.service)
    std::string controllers;  // enabled for children, e.g. "memory pids"
    std::string reason;       // why it is off, when it is
};

// Reads FUNES_CGROUPS; when it is 1, calls setup(). Once, at startup, before
// any child is started.
Status init();

// Moves this process (every pid in its cgroup) into `<own>/server` and
// enables the delegated controllers for children. Fails without changing
// anything when the cgroup is not writable. Exposed apart from init() so a
// test running inside a delegated scope can turn it on.
Status setup();

bool enabled();

// One child's cgroup. Created before the fork, the child joins it through the
// trampoline, finish() is called once it has been waited for.
class Scope {
public:
    // Null when cgroups are off or the directory could not be made — the
    // caller then runs the child the old way rather than not at all.
    static std::unique_ptr<Scope> create(const std::string& program, const Limits& limits);

    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    const std::string& path() const { return path_; }
    const std::string& name() const { return name_; }

    // cgroup.kill: SIGKILL to every process in it, including ones that left
    // the process group. Safe to call more than once.
    void kill();

    // Kills whatever is still in it, waits (briefly) for it to empty, reads
    // the counters and removes the directory.
    Usage finish();

private:
    Scope(std::string path, std::string name) : path_(std::move(path)), name_(std::move(name)) {}
    std::string path_;
    std::string name_;
    bool        finished_ = false;
};

} // namespace funes::cgroup
