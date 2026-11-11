// =============================================================================
// src/core/cgroup.cpp — see cgroup.h
// =============================================================================

#include "funes_config.h"
#include "cgroup.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace funes::cgroup {

namespace {

// Written once by setup(), before any child is started, and only read after.
std::atomic<bool> g_enabled{false};
std::string       g_dir;
bool              g_memory = false, g_pids = false, g_cpu = false;
std::atomic<uint64_t> g_seq{0};

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// One write() of the whole value: cgroup files take a value per write call,
// and a buffered stream is free to split it.
bool write_file(const std::string& path, const std::string& value) {
    FILE* f = std::fopen(path.c_str(), "we");
    if (!f) return false;
    setvbuf(f, nullptr, _IONBF, 0);
    const bool ok = std::fwrite(value.data(), 1, value.size(), f) == value.size();
    return std::fclose(f) == 0 && ok;
}

// "key value" lines (cpu.stat, memory.events, cgroup.events).
uint64_t keyed(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string k;
    uint64_t v = 0;
    while (in >> k >> v)
        if (k == key) return v;
    return 0;
}

uint64_t single(const std::string& path) {
    const std::string s = read_file(path);
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10);
}

std::string own_cgroup_dir() {
    std::ifstream in("/proc/self/cgroup");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("0::", 0) == 0) return "/sys/fs/cgroup" + line.substr(3);
    return "";
}

int64_t env_int(const char* name, int64_t fallback) {
    const char* v = funes::config_value(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const long long n = std::strtoll(v, &end, 10);
    return (end && *end == '\0') ? n : fallback;
}

// A name the cgroup filesystem and a log line both take: the program's
// basename, [A-Za-z0-9._-] only, at most 32 bytes.
std::string label_of(const std::string& program) {
    const size_t slash = program.find_last_of('/');
    const std::string base = slash == std::string::npos ? program : program.substr(slash + 1);
    std::string out;
    for (char c : base) {
        if (out.size() >= 32) break;
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') ? c : '_';
    }
    return out.empty() ? "child" : out;
}

bool populated(const std::string& path) {
    return keyed(read_file(path + "/cgroup.events"), "populated") != 0;
}

} // namespace

Limits default_limits() {
    Limits l;
    const int64_t mb = env_int("FUNES_CHILD_MEMORY_MB", 4096);
    l.memory_max_bytes = mb > 0 ? mb << 20 : 0;
    const int64_t pids = env_int("FUNES_CHILD_PIDS_MAX", 512);
    l.pids_max = pids > 0 ? pids : 0;
    const int64_t w = env_int("FUNES_CHILD_CPU_WEIGHT", 50);
    l.cpu_weight = w > 0 ? static_cast<int>(std::min<int64_t>(w, 10000)) : 0;
    return l;
}

std::string format_usage(const Usage& u) {
    char buf[160];
    std::snprintf(buf, sizeof buf, " {%s cpu=%.2fs mem=%.1fMB pids=%llu", u.name.c_str(),
                  static_cast<double>(u.cpu_usec) / 1e6,
                  static_cast<double>(u.memory_peak) / (1024.0 * 1024.0),
                  static_cast<unsigned long long>(u.pids_peak));
    std::string s = buf;
    if (u.oom_killed) s += " oom";
    if (u.killed_leftovers) s += " leftovers killed";
    return s + "}";
}

Status setup() {
    Status st;
    if (g_enabled) {
        st.enabled = true;
        st.dir = g_dir;
        st.controllers = std::string(g_memory ? "memory " : "") + (g_pids ? "pids " : "") + (g_cpu ? "cpu" : "");
        return st;
    }
    std::string own = own_cgroup_dir();
    if (own.empty()) { st.reason = "no cgroup v2 entry in /proc/self/cgroup"; return st; }
    if (own.size() > 7 && own.compare(own.size() - 7, 7, "/server") == 0) own.resize(own.size() - 7);
    st.dir = own;
    if (access((own + "/cgroup.procs").c_str(), W_OK) != 0
        || access((own + "/cgroup.subtree_control").c_str(), W_OK) != 0) {
        st.reason = own + " is not writable — the unit needs Delegate=yes";
        return st;
    }

    // Only a cgroup this process has to itself. Writable is not delegated: a
    // desktop session's app scope is the user's to write, and "move every pid
    // in it into server/" there reorganised a whole application under its
    // service manager, which then tore it down. A unit with Delegate=yes holds
    // the server and nothing else at startup.
    {
        std::istringstream procs(read_file(own + "/cgroup.procs"));
        size_t others = 0;
        for (pid_t pid; procs >> pid;) others += (pid != getpid());
        if (others > 0) {
            st.reason = own + " holds " + std::to_string(others) + " other process(es) — "
                        "run as a unit of its own with Delegate=yes";
            return st;
        }
    }

    // cgroup v2 allows processes only in leaves once controllers are enabled
    // for children, so the server leaves its own cgroup first.
    const std::string server = own + "/server";
    if (mkdir(server.c_str(), 0755) != 0 && errno != EEXIST) {
        st.reason = "cannot create " + server + ": " + std::strerror(errno);
        return st;
    }
    if (!write_file(server + "/cgroup.procs", std::to_string(getpid()))) {
        st.reason = "cannot move into " + server + ": " + std::strerror(errno);
        rmdir(server.c_str());
        return st;
    }

    std::istringstream avail(read_file(own + "/cgroup.controllers"));
    for (std::string c; avail >> c;) {
        if (c != "memory" && c != "pids" && c != "cpu") continue;
        if (!write_file(own + "/cgroup.subtree_control", "+" + c)) continue;
        if (c == "memory") g_memory = true;
        if (c == "pids")   g_pids = true;
        if (c == "cpu")    g_cpu = true;
        st.controllers += (st.controllers.empty() ? "" : " ") + c;
    }
    g_dir = own;
    g_enabled = true;
    st.enabled = true;
    return st;
}

Status init() {
    const char* v = funes::config_value("FUNES_CGROUPS");
    if (!v || std::string(v) != "1") {
        Status st;
        st.reason = "FUNES_CGROUPS is not 1";
        return st;
    }
    return setup();
}

bool enabled() { return g_enabled; }

std::unique_ptr<Scope> Scope::create(const std::string& program, const Limits& limits) {
    if (!g_enabled) return nullptr;
    const std::string name = "exec-" + std::to_string(++g_seq) + "-" + label_of(program);
    const std::string path = g_dir + "/" + name;
    if (mkdir(path.c_str(), 0755) != 0) {
        std::cerr << "[cgroup] cannot create " << path << ": " << std::strerror(errno)
                  << " — running the child without one\n";
        return nullptr;
    }
    if (g_memory) {
        // One OOM kill takes the whole call, not whichever process the kernel
        // picked while the rest carry on half-finished.
        write_file(path + "/memory.oom.group", "1");
        if (limits.memory_max_bytes > 0) {
            write_file(path + "/memory.max", std::to_string(limits.memory_max_bytes));
            write_file(path + "/memory.swap.max", "0");
        }
    }
    if (g_pids && limits.pids_max > 0)
        write_file(path + "/pids.max", std::to_string(limits.pids_max));
    if (g_cpu && limits.cpu_weight > 0)
        write_file(path + "/cpu.weight", std::to_string(limits.cpu_weight));
    return std::unique_ptr<Scope>(new Scope(path, name));
}

void Scope::kill() {
    if (write_file(path_ + "/cgroup.kill", "1")) return;
    // Before 5.14 there is no cgroup.kill; a pass over the member list misses
    // a process forked during it, which is why it is the fallback.
    std::istringstream procs(read_file(path_ + "/cgroup.procs"));
    for (pid_t pid; procs >> pid;) ::kill(pid, SIGKILL);
}

Usage Scope::finish() {
    Usage u;
    u.name = name_;
    if (finished_) return u;
    finished_ = true;

    if (populated(path_)) {
        u.killed_leftovers = true;
        kill();
    }
    // A SIGKILLed process leaves the cgroup when the kernel has reaped it,
    // not when the signal is sent; rmdir fails with EBUSY until then.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (populated(path_) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    u.cpu_usec    = keyed(read_file(path_ + "/cpu.stat"), "usage_usec");
    u.memory_peak = single(path_ + "/memory.peak");
    u.pids_peak   = single(path_ + "/pids.peak");
    u.oom_killed  = keyed(read_file(path_ + "/memory.events"), "oom_kill") > 0;

    for (int i = 0; i < 100 && rmdir(path_.c_str()) != 0 && errno == EBUSY; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return u;
}

Scope::~Scope() {
    if (!finished_) finish();
}

} // namespace funes::cgroup
