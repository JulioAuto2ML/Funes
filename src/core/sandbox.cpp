// =============================================================================
// src/core/sandbox.cpp — see sandbox.h
// =============================================================================
// Landlock's uapi is declared here rather than taken from <linux/landlock.h>:
// the build host's headers may predate the ABI the running kernel has (Debian
// 12's describe ABI 2, yoda's kernel speaks 6), and the values are a stable
// kernel ABI.

#include "funes_config.h"
#include "sandbox.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#define __NR_landlock_add_rule       445
#define __NR_landlock_restrict_self  446
#endif

namespace funes::sandbox {

namespace {

constexpr uint32_t kCreateRulesetVersion = 1u << 0;
constexpr int      kRulePathBeneath = 1;
constexpr int      kRuleNetPort     = 2;

// LANDLOCK_ACCESS_FS_*, by the ABI that introduced them.
constexpr uint64_t kExecute    = 1ull << 0;
constexpr uint64_t kWriteFile  = 1ull << 1;
constexpr uint64_t kReadFile   = 1ull << 2;
constexpr uint64_t kReadDir    = 1ull << 3;
constexpr uint64_t kFsAbi1     = (1ull << 13) - 1;  // bits 0..12
constexpr uint64_t kRefer      = 1ull << 13;        // ABI 2
constexpr uint64_t kTruncate   = 1ull << 14;        // ABI 3
constexpr uint64_t kIoctlDev   = 1ull << 15;        // ABI 5
// The rights that mean anything on a file rather than a directory; a rule on
// a file naming any other is EINVAL.
constexpr uint64_t kFileRights = kExecute | kWriteFile | kReadFile | kTruncate | kIoctlDev;

constexpr uint64_t kConnectTcp = 1ull << 1;         // LANDLOCK_ACCESS_NET_CONNECT_TCP, ABI 4
constexpr uint64_t kScopeAbstractUnix = 1ull << 0;  // ABI 6
constexpr uint64_t kScopeSignal       = 1ull << 1;  // ABI 6

struct RulesetAttr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;
    uint64_t scoped;
};

struct __attribute__((packed)) PathBeneathAttr {
    uint64_t allowed_access;
    int32_t  parent_fd;
};

struct NetPortAttr {
    uint64_t allowed_access;
    uint64_t port;
};

uint64_t fs_rights(int abi) {
    uint64_t r = kFsAbi1;
    if (abi >= 2) r |= kRefer;
    if (abi >= 3) r |= kTruncate;
    if (abi >= 5) r |= kIoctlDev;
    return r;
}

// Adds a path rule; a path that is not there is skipped, not an error.
std::string add_path(int ruleset, const std::string& path, uint64_t access, uint64_t handled) {
    const int fd = open(path.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) return {};
    struct stat st{};
    if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) access &= kFileRights;
    PathBeneathAttr attr{access & handled, fd};
    const long rc = syscall(__NR_landlock_add_rule, ruleset, kRulePathBeneath, &attr, 0);
    const int err = errno;
    close(fd);
    if (rc != 0) return "landlock rule for " + path + ": " + std::strerror(err);
    return {};
}

// The seccomp denylist. Each entry is a syscall no script or shell command
// needs and a sandbox escape or a kernel attack surface might: debugging other
// processes, changing the mount table, loading code into the kernel, the
// io_uring interface (much of the recent kernel CVE list), reading another
// process's memory. EPERM rather than a kill: a program that probes for one
// of these gets an answer it can handle.
std::string install_seccomp() {
    static const int kDenied[] = {
        __NR_ptrace, __NR_mount, __NR_umount2, __NR_pivot_root,
        __NR_bpf, __NR_keyctl, __NR_add_key, __NR_request_key,
        __NR_init_module, __NR_finit_module, __NR_delete_module,
        __NR_kexec_load,
#ifdef __NR_kexec_file_load
        __NR_kexec_file_load,
#endif
        __NR_perf_event_open, __NR_userfaultfd,
        __NR_process_vm_readv, __NR_process_vm_writev,
#ifdef __NR_io_uring_setup
        __NR_io_uring_setup, __NR_io_uring_enter, __NR_io_uring_register,
#endif
#ifdef __NR_open_tree
        __NR_open_tree, __NR_move_mount, __NR_fsopen, __NR_fsmount, __NR_fsconfig,
#endif
    };
#if defined(__x86_64__)
    constexpr uint32_t kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr uint32_t kArch = AUDIT_ARCH_AARCH64;
#else
    return {};  // no filter on an architecture this was not written for
#endif
    constexpr uint32_t kDeny = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
    std::vector<sock_filter> f;
    f.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    // Another ABI (32-bit compat) numbers syscalls differently, so the list
    // below would not mean what it says: refuse the whole call.
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0));
    f.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    f.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
#if defined(__x86_64__)
    // x32 syscalls are the x86_64 numbers with bit 30 set.
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
    f.push_back(BPF_STMT(BPF_RET | BPF_K, kDeny));
#endif
    for (int nr : kDenied) {
        f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 1));
        f.push_back(BPF_STMT(BPF_RET | BPF_K, kDeny));
    }
    f.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    sock_fprog prog{static_cast<unsigned short>(f.size()), f.data()};
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0)
        return std::string("seccomp: ") + std::strerror(errno);
    return {};
}

} // namespace

int abi() {
    static const int v = [] {
        const long r = syscall(__NR_landlock_create_ruleset, nullptr, 0, kCreateRulesetVersion);
        return r > 0 ? static_cast<int>(r) : 0;
    }();
    return v;
}

bool enabled() {
    const char* v = funes::config_value("FUNES_SANDBOX");
    return abi() > 0 && !(v && std::string(v) == "0");
}

Policy base() {
    Policy p;
    p.exec  = {"/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/libx32", "/opt"};
    // /run: resolv.conf is a symlink into it on systemd-resolved hosts, and
    // Landlock judges the file a symlink lands on.
    p.read  = {"/etc", "/run", "/proc", "/sys"};
    p.write = {"/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom", "/dev/tty"};
    return p;
}

Policy for_workspace(const std::filesystem::path& workspace) {
    Policy p = base();
    p.write.insert(p.write.begin(), workspace.string());
    return p;
}

std::vector<std::string> trampoline_args(const Policy& p) {
    std::vector<std::string> out = {"--sandbox"};
    for (const auto& s : p.read)  out.push_back("--ro=" + s);
    for (const auto& s : p.exec)  out.push_back("--rx=" + s);
    for (const auto& s : p.write) out.push_back("--rw=" + s);
    for (int port : p.connect)    out.push_back("--connect=" + std::to_string(port));
    if (!p.stage.empty()) {
        out.push_back("--stage=" + p.stage);
        for (const auto& s : p.overlay) out.push_back("--overlay=" + s);
    }
    return out;
}

std::string apply(const Policy& p) {
    const int v = abi();
    if (v <= 0) return "Landlock is not available on this kernel";
    // Required for an unprivileged process to restrict itself, and wanted
    // anyway: a setuid program run inside would otherwise gain what the
    // sandbox took away.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return std::string("no_new_privs: ") + std::strerror(errno);

    const uint64_t handled = fs_rights(v);
    RulesetAttr attr{handled, 0, 0};
    size_t size = offsetof(RulesetAttr, handled_access_net);
    if (v >= 4) {
        size = offsetof(RulesetAttr, scoped);
        if (!p.connect.empty()) attr.handled_access_net = kConnectTcp;
    }
    if (v >= 6) {
        size = sizeof(RulesetAttr);
        attr.scoped = kScopeAbstractUnix | kScopeSignal;
    }
    const long rs = syscall(__NR_landlock_create_ruleset, &attr, size, 0);
    if (rs < 0) return std::string("landlock ruleset: ") + std::strerror(errno);
    const int ruleset = static_cast<int>(rs);

    std::string err;
    for (const auto& s : p.read)
        if (err.empty()) err = add_path(ruleset, s, kReadFile | kReadDir, handled);
    for (const auto& s : p.exec)
        if (err.empty()) err = add_path(ruleset, s, kReadFile | kReadDir | kExecute, handled);
    for (const auto& s : p.write)
        if (err.empty()) err = add_path(ruleset, s, handled, handled);
    if (err.empty() && attr.handled_access_net) {
        for (int port : p.connect) {
            NetPortAttr np{kConnectTcp, static_cast<uint64_t>(port)};
            if (syscall(__NR_landlock_add_rule, ruleset, kRuleNetPort, &np, 0) != 0) {
                err = "landlock port " + std::to_string(port) + ": " + std::strerror(errno);
                break;
            }
        }
    }
    if (err.empty() && syscall(__NR_landlock_restrict_self, ruleset, 0) != 0)
        err = std::string("landlock restrict: ") + std::strerror(errno);
    close(ruleset);
    if (!err.empty()) return err;
    return install_seccomp();
}

} // namespace funes::sandbox
