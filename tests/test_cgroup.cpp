// =============================================================================
// tests/test_cgroup.cpp — a cgroup per child process (cgroup.h)
// =============================================================================
// Needs a delegated cgroup, which a test cannot make for itself: ctest runs
// it under `systemd-run --user --scope -p Delegate=yes` when that exists
// (tests/CMakeLists.txt). Anywhere else setup() fails and the test says SKIP
// rather than passing on the strength of having checked nothing.
//
// The binary is its own trampoline, as funes is: main() hands `_exec` to
// proc::trampoline_main before anything else.

#include "cgroup.h"
#include "tool_log.h"
#include "tools/process_runner.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

std::string own_cgroup() {
    std::ifstream in("/proc/self/cgroup");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("0::", 0) == 0) return line.substr(3);
    return "";
}

const fs::path kCwd = fs::temp_directory_path();

// Writable is not delegated. A test once run from a desktop terminal found
// the app's scope writable, moved all of the app's processes into server/,
// and the session manager tore the app down. A cgroup shared with anything
// else is refused before a single write.
int test_shared_cgroup_is_refused() {
    const std::string before = own_cgroup();
    const pid_t other = fork();
    if (other == 0) { execlp("sleep", "sleep", "30", (char*)nullptr); _exit(127); }
    CHECK(other > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto st = funes::cgroup::setup();
    ::kill(other, SIGKILL);
    waitpid(other, nullptr, 0);
    CHECK(!st.enabled);
    CHECK(st.reason.find("other process") != std::string::npos);
    CHECK(own_cgroup() == before);
    CHECK(!fs::exists("/sys/fs/cgroup" + before + "/server"));
    return 0;
}

int test_server_moves_into_its_own_leaf(const funes::cgroup::Status& st) {
    CHECK(st.enabled);
    const std::string mine = own_cgroup();
    CHECK(mine.size() > 7 && mine.substr(mine.size() - 7) == "/server");
    CHECK(st.controllers.find("pids") != std::string::npos);
    CHECK(st.controllers.find("memory") != std::string::npos);
    return 0;
}

int test_child_runs_in_a_cgroup_of_its_own() {
    auto r = funes::proc::run_argv({"cat", "/proc/self/cgroup"}, kCwd, 10, 4096);
    CHECK(r.exit_code == 0);
    CHECK(r.output.find("/exec-") != std::string::npos);
    CHECK(r.output.find("/server") == std::string::npos);
    CHECK(r.usage.has_value());
    CHECK(r.usage->name.rfind("exec-", 0) == 0);
    CHECK(r.usage->name.find("cat") != std::string::npos);
    // Removed once it was waited for: the parent directory has only server/
    // and whatever another test left behind would show here.
    const std::string parent = "/sys/fs/cgroup" + own_cgroup().substr(0, own_cgroup().size() - 7);
    CHECK(!fs::exists(parent + "/" + r.usage->name));
    return 0;
}

int test_shell_runs_in_a_cgroup_too() {
    auto r = funes::proc::run_shell_command("cat /proc/self/cgroup", kCwd, 10, 4096);
    CHECK(r.exit_code == 0);
    CHECK(r.output.find("/exec-") != std::string::npos);
    CHECK(r.usage.has_value());
    return 0;
}

// The case kill(-pgid) cannot handle: a grandchild in its own session. With
// only the process group to go on it outlives the call by five minutes.
int test_setsid_grandchild_does_not_outlive_the_call() {
    auto r = funes::proc::run_shell_command("setsid sleep 300 >/dev/null 2>&1 & echo $!",
                                            kCwd, 10, 4096);
    CHECK(r.exit_code == 0);
    const pid_t escaped = static_cast<pid_t>(std::stol(r.output));
    CHECK(escaped > 0);
    CHECK(r.usage.has_value());
    CHECK(r.usage->killed_leftovers);
    // finish() waits for the cgroup to empty, so it is gone, not dying.
    errno = 0;
    CHECK(::kill(escaped, 0) != 0 && errno == ESRCH);
    return 0;
}

int test_timeout_kills_the_whole_cgroup() {
    auto r = funes::proc::run_shell_command("setsid sleep 300 & sleep 300", kCwd, 1, 4096);
    CHECK(r.timed_out);
    CHECK(r.usage.has_value());
    return 0;
}

int test_pids_max_is_enforced() {
    funes::cgroup::Limits lim;
    lim.pids_max = 4;
    auto r = funes::proc::run_shell_command(
        "for i in 1 2 3 4 5 6 7 8 9 10; do sleep 1 & done; wait", kCwd, 20, 4096, lim);
    CHECK(r.usage.has_value());
    CHECK(r.usage->pids_peak > 0 && r.usage->pids_peak <= 4);
    // The shell reports the forks it could not make.
    CHECK(r.output.find("fork") != std::string::npos || r.output.find("Resource") != std::string::npos);
    return 0;
}

int test_memory_max_ends_in_an_oom_kill() {
    funes::cgroup::Limits lim;
    lim.memory_max_bytes = 32ll << 20;
    // tail keeps a line it has not seen the end of; /dev/zero has no newline.
    auto r = funes::proc::run_shell_command("head -c 400000000 /dev/zero | tail -n 1 >/dev/null",
                                            kCwd, 30, 4096, lim);
    CHECK(r.usage.has_value());
    CHECK(r.usage->oom_killed);
    CHECK(r.exit_code != 0);
    CHECK(r.usage->memory_peak > 0 && r.usage->memory_peak <= (33ull << 20));
    return 0;
}

int test_usage_is_counted() {
    auto r = funes::proc::run_shell_command(
        "i=0; while [ $i -lt 200000 ]; do i=$((i+1)); done; echo $i", kCwd, 30, 4096);
    CHECK(r.exit_code == 0);
    CHECK(r.usage.has_value());
    CHECK(r.usage->cpu_usec > 1000);
    CHECK(r.usage->memory_peak > 0);
    CHECK(!r.usage->oom_killed);
    CHECK(!r.usage->killed_leftovers);
    return 0;
}

// Two threads at once, the way two chats or a chat and the cron thread would
// start children: every one gets its own cgroup and every cgroup is removed.
int test_concurrent_children_get_distinct_cgroups() {
    std::string a, b;
    std::thread t1([&] { a = funes::proc::run_argv({"true"}, kCwd, 10, 1024).usage.value_or(funes::cgroup::Usage{}).name; });
    std::thread t2([&] { b = funes::proc::run_argv({"true"}, kCwd, 10, 1024).usage.value_or(funes::cgroup::Usage{}).name; });
    t1.join();
    t2.join();
    CHECK(!a.empty() && !b.empty() && a != b);
    return 0;
}

int test_format_usage() {
    funes::cgroup::Usage u;
    u.name = "exec-3-sh";
    u.cpu_usec = 310000;
    u.memory_peak = 13 * 1024 * 1024;
    u.pids_peak = 3;
    const std::string s = funes::cgroup::format_usage(u);
    CHECK(s == " {exec-3-sh cpu=0.31s mem=13.0MB pids=3}");
    u.oom_killed = true;
    u.killed_leftovers = true;
    CHECK(funes::cgroup::format_usage(u) == " {exec-3-sh cpu=0.31s mem=13.0MB pids=3 oom leftovers killed}");
    return 0;
}

int test_tool_line_carries_usage() {
    const std::string line = funes::format_tool_call_line(
        "operator", 0, "run_script:tv_art", "{}", funes::ToolCallOutcome::Ok, "", 160,
        " {exec-3-python3 cpu=0.31s mem=13.0MB pids=3}");
    CHECK(line == "[tool] operator (depth 0) run_script:tv_art args={} → ok"
                  " {exec-3-python3 cpu=0.31s mem=13.0MB pids=3}\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "_exec")
        return funes::proc::trampoline_main(argc, argv);

    int failed = 0;
    failed += test_format_usage();
    failed += test_tool_line_carries_usage();

    funes::proc::set_trampoline("/proc/self/exe");
    const bool delegated = access(("/sys/fs/cgroup" + own_cgroup() + "/cgroup.subtree_control").c_str(), W_OK) == 0;
    if (delegated) failed += test_shared_cgroup_is_refused();
    const auto st = funes::cgroup::setup();
    if (!st.enabled) {
        std::cout << "SKIP: no delegated cgroup (" << st.reason << ")\n";
        return failed ? 1 : 0;
    }
    failed += test_server_moves_into_its_own_leaf(st);
    failed += test_child_runs_in_a_cgroup_of_its_own();
    failed += test_shell_runs_in_a_cgroup_too();
    failed += test_setsid_grandchild_does_not_outlive_the_call();
    failed += test_timeout_kills_the_whole_cgroup();
    failed += test_pids_max_is_enforced();
    failed += test_memory_max_ends_in_an_oom_kill();
    failed += test_usage_is_counted();
    failed += test_concurrent_children_get_distinct_cgroups();
    if (failed) { std::cerr << failed << " test(s) failed\n"; return 1; }
    std::cout << "cgroup: all tests passed\n";
    return 0;
}
