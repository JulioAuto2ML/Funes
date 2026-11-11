// =============================================================================
// tests/test_sandbox.cpp — Landlock + seccomp on child processes (sandbox.h)
// =============================================================================
// The point of phase 2 is that the *kernel* refuses, so every case here goes
// through proc::run_argv / run_shell_command (or the tools that call them)
// with fs_guard nowhere in the path, and most have a control: the same command
// without a policy succeeds, so a pass means the sandbox refused it and not
// that the command could never have worked.
//
// The binary is its own trampoline, as funes is, and its own probe: `_probe
// ptrace` and `_probe connect PORT` report one syscall's errno.

#include "sandbox.h"
#include "tools.h"
#include "tools/process_runner.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const fs::path kBase = fs::temp_directory_path() / "funes_sandbox_test";
const fs::path kWs1  = kBase / "workspace" / "1";
const fs::path kWs2  = kBase / "workspace" / "2";
const fs::path kConf = kBase / "config" / "funes.local";
const fs::path kDb   = kBase / "data" / "memory.db";

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

void build_tree() {
    fs::remove_all(kBase);
    fs::create_directories(kWs1);
    write(kWs2 / "secret.txt", "account two's file\n");
    write(kConf, "FUNES_SERVICE_TOKEN=s3cret\n");
    write(kDb, "SQLite format 3\n");
}

std::string self_dir() { return fs::read_symlink("/proc/self/exe").parent_path().string(); }

funes::proc::Result sh(const std::string& cmd, bool sandboxed) {
    return funes::proc::run_shell_command(
        cmd, kWs1, 10, 8192, std::nullopt,
        sandboxed ? std::optional<funes::sandbox::Policy>(funes::sandbox::for_workspace(kWs1))
                  : std::nullopt);
}

int test_workspace_is_read_write() {
    auto r = sh("echo hi > out.txt && cat out.txt && mkdir -p sub && rm out.txt", true);
    CHECK(r.exit_code == 0);
    CHECK(r.output == "hi\n");
    return 0;
}

int test_secrets_are_refused_by_the_kernel() {
    for (const fs::path& p : {kConf, kWs2 / "secret.txt", kDb}) {
        auto control = sh("cat '" + p.string() + "'", false);
        CHECK(control.exit_code == 0);
        auto r = sh("cat '" + p.string() + "'", true);
        CHECK(r.exit_code != 0);
        CHECK(r.output.find("Permission denied") != std::string::npos);
    }
    // Not listable either: a directory name is information.
    CHECK(sh("ls '" + kWs2.string() + "'", true).exit_code != 0);
    return 0;
}

int test_nothing_is_written_outside() {
    const fs::path escape = kBase / "escape.txt";
    auto r = sh("echo x > '" + escape.string() + "'", true);
    CHECK(r.exit_code != 0);
    CHECK(!fs::exists(escape));
    CHECK(sh("rm '" + kConf.string() + "'", true).exit_code != 0);
    CHECK(fs::exists(kConf));
    return 0;
}

int test_private_tmpdir() {
    auto r = sh("echo x > \"$TMPDIR/f\" && cat \"$TMPDIR/f\" && echo \"$TMPDIR\"", true);
    CHECK(r.exit_code == 0);
    const std::string dir = r.output.substr(r.output.find('\n') + 1, r.output.size() - r.output.find('\n') - 2);
    CHECK(dir.rfind("/tmp/funes-exec-", 0) == 0 || dir.find("/funes-exec-") != std::string::npos);
    CHECK(!fs::exists(dir));  // removed after the call
    // /tmp itself is not shared between calls (or accounts).
    CHECK(sh("echo x > /tmp/funes_sandbox_shared", true).exit_code != 0);
    return 0;
}

// /proc/<pid>/environ of the process that started the child is where the
// server's secrets live (funes.local is loaded into its environment). Same
// uid, and yoda's yama ptrace_scope is 0 — only Landlock's ptrace rule stops it.
int test_parent_environment_is_unreadable() {
    // /proc/<pid>/environ is the block the process was exec'd with (a later
    // setenv does not show), which is exactly where funes.local's values are
    // not — but PATH is, and stands in for them.
    const std::string cmd = "cat /proc/" + std::to_string(getpid()) + "/environ";
    CHECK(sh(cmd, false).output.find("PATH=") != std::string::npos);
    auto r = sh(cmd, true);
    CHECK(r.output.find("PATH=") == std::string::npos);
    CHECK(r.exit_code != 0);
    // funes.local's secrets are setenv'd, so they live in the heap — and
    // /proc/<pid>/mem is how a same-uid process reads that when yama's
    // ptrace_scope is 0 (yoda's setting). Opening it is the whole question.
    const std::string mem = "exec 3< /proc/" + std::to_string(getpid()) + "/mem && echo opened";
    const auto open_control = sh(mem, false);
    if (open_control.output == "opened\n") {
        auto m = sh(mem, true);
        CHECK(m.exit_code != 0);
        CHECK(m.output.find("opened") == std::string::npos);
    } else {
        std::cout << "  (ptrace_scope already refuses /proc/<pid>/mem here — control not meaningful)\n";
    }
    return 0;
}

int test_seccomp_denies_ptrace() {
    funes::sandbox::Policy p = funes::sandbox::for_workspace(kWs1);
    p.exec.push_back(self_dir());
    const std::vector<std::string> argv = {"/proc/self/exe", "_probe", "ptrace"};
    // /proc/self/exe resolves in the child to this binary.
    auto control = funes::proc::run_argv(argv, kWs1, 10, 1024);
    CHECK(control.output == "ok\n");
    auto r = funes::proc::run_argv(argv, kWs1, 10, 1024, {}, false, std::nullopt, p);
    CHECK(r.output == "EPERM\n");
    return 0;
}

int test_signals_are_scoped() {
    if (funes::sandbox::abi() < 6) { std::cout << "  (signal scoping needs Landlock ABI 6 — skipped)\n"; return 0; }
    const std::string cmd = "kill -0 " + std::to_string(getpid());
    CHECK(sh(cmd, false).exit_code == 0);
    CHECK(sh(cmd, true).exit_code != 0);
    return 0;
}

int test_connect_ports() {
    if (funes::sandbox::abi() < 4) { std::cout << "  (TCP rules need Landlock ABI 4 — skipped)\n"; return 0; }
    const int ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    CHECK(listen(ls, 4) == 0);
    socklen_t len = sizeof a;
    getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
    const int port = ntohs(a.sin_port);

    funes::sandbox::Policy p = funes::sandbox::for_workspace(kWs1);
    p.exec.push_back(self_dir());
    const std::vector<std::string> argv = {"/proc/self/exe", "_probe", "connect", std::to_string(port)};
    CHECK(funes::proc::run_argv(argv, kWs1, 10, 1024, {}, false, std::nullopt, p).output == "ok\n");
    p.connect = {443};
    CHECK(funes::proc::run_argv(argv, kWs1, 10, 1024, {}, false, std::nullopt, p).output == "EACCES\n");
    p.connect = {443, port};
    CHECK(funes::proc::run_argv(argv, kWs1, 10, 1024, {}, false, std::nullopt, p).output == "ok\n");
    close(ls);
    return 0;
}

// The tools themselves, not just the runner: execute_shell is confined to the
// caller's workspace, and a script gets its manifest's `access:` and no more.
int test_execute_shell_is_confined() {
    setenv("FUNES_ALLOW_SHELL", "1", 1);
    ToolRegistry reg;
    register_shell_tool(reg, (kBase / "workspace").string());
    ToolContext one{"funes", "s", "", "", 1};
    auto own = reg.call("execute_shell", {{"command", "echo mine > a.txt && cat a.txt"}}, one);
    CHECK(!own.error);
    auto theirs = reg.call("execute_shell", {{"command", "cat ../2/secret.txt"}}, one);
    CHECK(theirs.error);
    CHECK(theirs.text.find("Permission denied") != std::string::npos);
    return 0;
}

int test_script_gets_its_declared_access() {
    const fs::path lib = kBase / "scriptlib";
    const fs::path shared = kBase / "data" / "tv";
    fs::create_directories(shared);
    write(lib / "store.sh", "#!/bin/sh\necho ok > \"$1\" && echo stored\n");
    fs::permissions(lib / "store.sh", fs::perms::owner_all);
    write(lib / "store.yaml",
          "description: x\nrun: store.sh\nparams:\n  - name: to\n    required: true\n"
          "access:\n  write: [" + shared.string() + "]\n");
    // The script takes `--to=PATH`; strip the prefix in the shell.
    write(lib / "store.sh", "#!/bin/sh\nf=\"${1#--to=}\"\necho ok > \"$f\" && echo stored\n");
    fs::permissions(lib / "store.sh", fs::perms::owner_all);

    ToolRegistry reg;
    register_script_tools(reg, (kBase / "workspace").string(), lib.string());
    ToolContext one{"funes", "s", "", "", 1};
    one.scripts = {"store"};
    auto ok = reg.call("run_script", {{"name", "store"}, {"arguments", {{"to", (shared / "f").string()}}}}, one);
    CHECK(!ok.error);
    CHECK(fs::exists(shared / "f"));
    auto no = reg.call("run_script", {{"name", "store"}, {"arguments", {{"to", kConf.string()}}}}, one);
    CHECK(no.error);
    std::ifstream in(kConf);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    CHECK(content == "FUNES_SERVICE_TOKEN=s3cret\n");
    return 0;
}

int probe(int argc, char** argv) {
    const std::string what = argc > 2 ? argv[2] : "";
    int rc = -1;
    if (what == "ptrace") {
        rc = static_cast<int>(ptrace(PTRACE_TRACEME, 0, nullptr, nullptr));
    } else if (what == "connect" && argc > 3) {
        const int s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<uint16_t>(std::atoi(argv[3])));
        rc = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    if (rc == 0) std::cout << "ok\n";
    else if (errno == EPERM) std::cout << "EPERM\n";
    else if (errno == EACCES) std::cout << "EACCES\n";
    else std::cout << std::strerror(errno) << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "_exec")
        return funes::proc::trampoline_main(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "_probe")
        return probe(argc, argv);

    if (!funes::sandbox::enabled()) {
        std::cout << "SKIP: no Landlock on this kernel (or FUNES_SANDBOX=0)\n";
        return 0;
    }
    std::cout << "Landlock ABI " << funes::sandbox::abi() << "\n";
    funes::proc::set_trampoline("/proc/self/exe");
    build_tree();

    int failed = 0;
    failed += test_workspace_is_read_write();
    failed += test_secrets_are_refused_by_the_kernel();
    failed += test_nothing_is_written_outside();
    failed += test_private_tmpdir();
    failed += test_parent_environment_is_unreadable();
    failed += test_seccomp_denies_ptrace();
    failed += test_signals_are_scoped();
    failed += test_connect_ports();
    failed += test_execute_shell_is_confined();
    failed += test_script_gets_its_declared_access();
    fs::remove_all(kBase);
    if (failed) { std::cerr << failed << " test(s) failed\n"; return 1; }
    std::cout << "sandbox: all tests passed\n";
    return 0;
}
