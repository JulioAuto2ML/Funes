// =============================================================================
// src/core/sandbox.h — Landlock + seccomp for child processes (OS plan, phase 2)
// =============================================================================
// Until 7.5, what a script or shell command could touch was decided by a path
// check in Funes's own code (fs_guard) — which only the file tools go through.
// A script ran with the server's whole filesystem: config/funes.local, every
// account's workspace, memory.db. scriptlib/README.md said "an allowlist, not
// a sandbox" and operator's prompt had to warn that execute_shell was not
// confined. A prompt was carrying a security boundary.
//
// Now the grant is the sandbox. The `funes _exec` trampoline
// (process_runner.h) applies a Landlock ruleset and a seccomp filter to itself
// and then execs the program, so the kernel enforces it, the program inherits
// it, and nothing it starts can drop it:
//
//   - read + execute: the system (/usr, /etc, /lib*, /bin, /sbin, /opt, /run),
//     /proc and /sys read-only, a few /dev nodes
//   - read-write: the caller's workspace and a private temp directory made for
//     this one call (TMPDIR points at it; it is deleted afterwards). /tmp itself
//     is not granted — a shared /tmp would be a path from one account's files
//     to another's
//   - whatever a script's manifest adds under `access:` (read / write / exec)
//   - TCP connect only to the manifest's `connect:` ports, when it lists any;
//     otherwise the network is not restricted (Landlock rules are per port,
//     not per address — net_guard is still what judges hosts)
//   - signals and abstract unix sockets scoped to the sandbox (ABI 6)
//   - ptrace into anything outside the sandbox refused — which is also what
//     keeps /proc/<server pid>/mem unopenable: funes.local is setenv'd into
//     the server's heap, and with yama's ptrace_scope at 0 (yoda) any process
//     of the same user could otherwise read it from there
//   - a seccomp denylist: ptrace, mount, bpf, keyctl, module loading,
//     io_uring, kexec, perf_event_open, userfaultfd, process_vm_*
//
// Rights are masked against the running kernel's Landlock ABI, so an older
// kernel enforces less instead of failing. Unavailable (no Landlock in the LSM
// list) or FUNES_SANDBOX=0, children run unconfined, and startup says so.

#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace funes::sandbox {

struct Policy {
    std::vector<std::string> read;     // read files, list directories
    std::vector<std::string> exec;     // read + execute
    std::vector<std::string> write;    // everything, including execute
    std::vector<int>         connect;  // TCP ports; empty = network unrestricted
};

// The running kernel's Landlock ABI version; 0 when it has none.
int abi();

// abi() > 0 and FUNES_SANDBOX is not 0.
bool enabled();

// What every child gets: the system read-only/executable and the harmless
// /dev nodes. A private temp directory is added by the process runner.
Policy base();

// base() plus `workspace` read-write — scripts and shell commands.
Policy for_workspace(const std::filesystem::path& workspace);

// The trampoline's options for `p`: --ro=, --rx=, --rw=, --connect=.
std::vector<std::string> trampoline_args(const Policy& p);

// In the trampoline, after the cgroup is joined and before exec: set
// no_new_privs, restrict this process with `p`, install the seccomp filter.
// A path in `p` that does not exist is skipped (a grant that cannot be
// applied grants nothing). Returns an empty string, or what failed — and then
// the trampoline does not exec: a child that was meant to be confined does
// not run unconfined.
std::string apply(const Policy& p);

} // namespace funes::sandbox
