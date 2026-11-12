// =============================================================================
// src/core/observe.cpp — see observe.h
// =============================================================================

#include "observe.h"
#include "funes_config.h"
#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace funes::observe {

// ── records ──────────────────────────────────────────────────────────────────

std::string to_json_line(const Access& a) {
    json j = {{"op", a.op}};
    if (!a.path.empty())   j["path"] = a.path;
    if (!a.family.empty()) j["family"] = a.family;
    if (!a.proto.empty())  j["proto"] = a.proto;
    if (a.port)            j["port"] = a.port;
    if (!a.addr.empty())   j["addr"] = a.addr;
    return j.dump();
}

std::vector<Access> parse_lines(const std::string& text) {
    std::vector<Access> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        try {
            const json j = json::parse(line);
            if (!j.is_object() || !j.contains("op") || !j["op"].is_string()) continue;
            Access a;
            a.op = j["op"].get<std::string>();
            if (j.contains("path") && j["path"].is_string())     a.path = j["path"];
            if (j.contains("family") && j["family"].is_string()) a.family = j["family"];
            if (j.contains("proto") && j["proto"].is_string())   a.proto = j["proto"];
            if (j.contains("port") && j["port"].is_number_integer()) a.port = j["port"];
            if (j.contains("addr") && j["addr"].is_string())     a.addr = j["addr"];
            out.push_back(std::move(a));
        } catch (const std::exception&) {}
    }
    return out;
}

namespace {

bool under(const std::string& path, const std::string& root) {
    if (root.empty()) return false;
    if (root == "/") return !path.empty() && path[0] == '/';
    return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0
                            && path[root.size()] == '/');
}

} // namespace

void rebase(std::vector<Access>& seen, const std::string& workspace) {
    if (workspace.empty()) return;
    std::error_code ec;
    std::string ws = fs::weakly_canonical(workspace, ec).string();
    if (ec || ws.empty()) ws = workspace;
    for (auto& a : seen)
        if (under(a.path, ws)) a.path = "${WORKSPACE}" + a.path.substr(ws.size());
}

// ── the supervisor ───────────────────────────────────────────────────────────

namespace {

// The syscalls that name a path or an address. Each is stopped until the
// supervisor has read its arguments, then continued unchanged.
const int kWatched[] = {
#ifdef __NR_open
    __NR_open,
#endif
#ifdef __NR_creat
    __NR_creat,
#endif
#ifdef __NR_mkdir
    __NR_mkdir,
#endif
#ifdef __NR_rmdir
    __NR_rmdir,
#endif
#ifdef __NR_unlink
    __NR_unlink,
#endif
#ifdef __NR_rename
    __NR_rename,
#endif
#ifdef __NR_link
    __NR_link,
#endif
#ifdef __NR_symlink
    __NR_symlink,
#endif
#ifdef __NR_openat2
    __NR_openat2,
#endif
#ifdef __NR_renameat
    __NR_renameat,
#endif
    __NR_openat, __NR_execve, __NR_execveat, __NR_connect, __NR_mkdirat,
    __NR_unlinkat, __NR_renameat2, __NR_linkat, __NR_symlinkat, __NR_truncate,
};

int install_listener() {
#if defined(__x86_64__)
    constexpr uint32_t kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr uint32_t kArch = AUDIT_ARCH_AARCH64;
#else
    errno = ENOSYS;
    return -1;
#endif
    std::vector<sock_filter> f;
    f.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    // Another ABI's numbers would not mean these syscalls; the sandbox's own
    // filter refuses that ABI outright, so here it is simply not watched.
    f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0));
    f.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    f.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
    for (int nr : kWatched) {
        f.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(nr), 0, 1));
        f.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF));
    }
    f.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    sock_fprog prog{static_cast<unsigned short>(f.size()), f.data()};
    return static_cast<int>(syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
                                    SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog));
}

bool send_fd(int sock, int fd) {
    char dummy = 'x';
    iovec io{&dummy, 1};
    alignas(cmsghdr) char buf[CMSG_SPACE(sizeof(int))] = {};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof buf;
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) == 1;
}

int recv_fd(int sock) {
    char dummy;
    iovec io{&dummy, 1};
    alignas(cmsghdr) char buf[CMSG_SPACE(sizeof(int))] = {};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof buf;
    if (recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) != 1) return -1;
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_type != SCM_RIGHTS) return -1;
    int fd;
    std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

// The traced task's memory, page by page so a string near the end of a
// mapping does not fail the whole read.
std::string read_string(int mem, uint64_t addr, size_t max = 4096) {
    std::string out;
    if (!addr) return out;
    char buf[512];
    while (out.size() < max) {
        const uint64_t at = addr + out.size();
        const size_t room = std::min<size_t>(sizeof buf, 4096 - (at % 4096));
        const ssize_t n = pread(mem, buf, room, static_cast<off_t>(at));
        if (n <= 0) break;
        const void* nul = std::memchr(buf, 0, static_cast<size_t>(n));
        if (nul) { out.append(buf, static_cast<const char*>(nul) - buf); return out; }
        out.append(buf, static_cast<size_t>(n));
    }
    return {};   // no terminator within reach: not a path we can trust to mean anything
}

bool read_bytes(int mem, uint64_t addr, void* into, size_t len) {
    return addr && pread(mem, into, len, static_cast<off_t>(addr)) == static_cast<ssize_t>(len);
}

std::string read_link(const std::string& p) {
    char buf[4096];
    const ssize_t n = readlink(p.c_str(), buf, sizeof buf);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

// `raw` as the task meant it: relative to its cwd or to `dirfd`, with
// /proc/self pointing at it rather than at us, then resolved as Landlock
// would see it. Empty when it cannot be placed.
std::string resolve(pid_t tid, int dirfd, const std::string& raw) {
    if (raw.empty()) return {};
    std::string p = raw;
    if (p[0] != '/') {
        const std::string base = dirfd == AT_FDCWD
            ? read_link("/proc/" + std::to_string(tid) + "/cwd")
            : read_link("/proc/" + std::to_string(tid) + "/fd/" + std::to_string(dirfd));
        if (base.empty() || base[0] != '/') return {};
        p = base + "/" + p;
    }
    for (const char* self : {"/proc/self", "/proc/thread-self"}) {
        const size_t n = std::strlen(self);
        if (p.compare(0, n, self) == 0 && (p.size() == n || p[n] == '/'))
            p = "/proc/" + std::to_string(tid) + p.substr(n);
    }
    std::error_code ec;
    fs::path r = fs::weakly_canonical(p, ec);
    if (ec) r = fs::path(p).lexically_normal();
    std::string s = r.string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

bool exists(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0;
}

std::string socket_proto(pid_t tid, int fd) {
    // The socket's type is not in connect()'s arguments; borrow the fd.
    pid_t tgid = tid;
    std::ifstream status("/proc/" + std::to_string(tid) + "/status");
    for (std::string line; std::getline(status, line);)
        if (line.rfind("Tgid:", 0) == 0) { tgid = std::atoi(line.c_str() + 5); break; }
    const int pidfd = static_cast<int>(syscall(__NR_pidfd_open, tgid, 0));
    if (pidfd < 0) return {};
    const int local = static_cast<int>(syscall(__NR_pidfd_getfd, pidfd, fd, 0));
    close(pidfd);
    if (local < 0) return {};
    int type = 0;
    socklen_t len = sizeof type;
    const bool ok = getsockopt(local, SOL_SOCKET, SO_TYPE, &type, &len) == 0;
    close(local);
    if (!ok) return {};
    return type == SOCK_STREAM ? "tcp" : type == SOCK_DGRAM ? "udp" : "";
}

// open()-family flags → what the call asks for.
std::string open_op(uint64_t flags, bool present) {
    if (flags & O_PATH) return {};               // no access right is checked for O_PATH
    if ((flags & O_CREAT) && !present) return "create";
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC)) return "write";
    return "read";
}

void decode(const seccomp_notif& n, int mem, std::vector<Access>& out) {
    const pid_t tid = static_cast<pid_t>(n.pid);
    const auto* a = n.data.args;
    const int nr = n.data.nr;
    auto path_at = [&](int dirfd, uint64_t addr) {
        return resolve(tid, dirfd, read_string(mem, addr));
    };
    auto add = [&](const std::string& op, const std::string& path) {
        if (op.empty() || path.empty()) return;
        if (under(path, "/proc") || under(path, "/sys")) return;   // the base reads both
        Access x;
        x.op = op;
        x.path = path;
        out.push_back(std::move(x));
    };
    auto add_open = [&](int dirfd, uint64_t addr, uint64_t flags) {
        const std::string p = path_at(dirfd, addr);
        if (!p.empty()) add(open_op(flags, exists(p)), p);
    };
    auto add_entry = [&](int dirfd, uint64_t addr, const std::string& op) {
        add(op, path_at(dirfd, addr));
    };
    // mkdir/link/symlink onto something that is there fails with EEXIST and
    // creates nothing — `mkdir -p` does it for every directory on the way.
    auto add_new = [&](int dirfd, uint64_t addr) {
        const std::string p = path_at(dirfd, addr);
        if (!p.empty() && !exists(p)) add("create", p);
    };

    if (nr == __NR_openat) add_open(static_cast<int>(a[0]), a[1], a[2]);
#ifdef __NR_open
    else if (nr == __NR_open) add_open(AT_FDCWD, a[0], a[1]);
#endif
#ifdef __NR_creat
    else if (nr == __NR_creat) add_open(AT_FDCWD, a[0], O_CREAT | O_WRONLY | O_TRUNC);
#endif
#ifdef __NR_openat2
    else if (nr == __NR_openat2) {
        uint64_t flags = 0;
        if (read_bytes(mem, a[2], &flags, sizeof flags)) add_open(static_cast<int>(a[0]), a[1], flags);
    }
#endif
    else if (nr == __NR_execve) add_entry(AT_FDCWD, a[0], "exec");
    else if (nr == __NR_execveat) add_entry(static_cast<int>(a[0]), a[1], "exec");
    else if (nr == __NR_truncate) add_entry(AT_FDCWD, a[0], "write");
    else if (nr == __NR_mkdirat) add_new(static_cast<int>(a[0]), a[1]);
    else if (nr == __NR_unlinkat) add_entry(static_cast<int>(a[0]), a[1], "remove");
    else if (nr == __NR_symlinkat) add_new(static_cast<int>(a[1]), a[2]);
    else if (nr == __NR_linkat) add_new(static_cast<int>(a[2]), a[3]);
    else if (nr == __NR_renameat2
#ifdef __NR_renameat
             || nr == __NR_renameat
#endif
             ) {
        add_entry(static_cast<int>(a[0]), a[1], "remove");
        add_entry(static_cast<int>(a[2]), a[3], "create");
    }
#ifdef __NR_mkdir
    else if (nr == __NR_mkdir) add_new(AT_FDCWD, a[0]);
#endif
#ifdef __NR_rmdir
    else if (nr == __NR_rmdir) add_entry(AT_FDCWD, a[0], "remove");
#endif
#ifdef __NR_unlink
    else if (nr == __NR_unlink) add_entry(AT_FDCWD, a[0], "remove");
#endif
#ifdef __NR_rename
    else if (nr == __NR_rename) {
        add_entry(AT_FDCWD, a[0], "remove");
        add_entry(AT_FDCWD, a[1], "create");
    }
#endif
#ifdef __NR_link
    else if (nr == __NR_link) add_new(AT_FDCWD, a[1]);
#endif
#ifdef __NR_symlink
    else if (nr == __NR_symlink) add_new(AT_FDCWD, a[1]);
#endif
    else if (nr == __NR_connect) {
        sockaddr_storage ss{};
        const size_t len = std::min<size_t>(a[2], sizeof ss);
        if (len < sizeof(sa_family_t) || !read_bytes(mem, a[1], &ss, len)) return;
        Access x;
        x.op = "connect";
        if (ss.ss_family == AF_INET && len >= sizeof(sockaddr_in)) {
            const auto* in = reinterpret_cast<const sockaddr_in*>(&ss);
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf);
            x.family = "inet";
            x.port = ntohs(in->sin_port);
            x.addr = buf;
        } else if (ss.ss_family == AF_INET6 && len >= sizeof(sockaddr_in6)) {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&ss);
            char buf[INET6_ADDRSTRLEN] = {};
            inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof buf);
            x.family = "inet";
            x.port = ntohs(in6->sin6_port);
            x.addr = buf;
        } else if (ss.ss_family == AF_UNIX) {
            const auto* un = reinterpret_cast<const sockaddr_un*>(&ss);
            const size_t plen = len - offsetof(sockaddr_un, sun_path);
            if (plen == 0) return;
            if (un->sun_path[0] == '\0')
                x.path = "@" + std::string(un->sun_path + 1, strnlen(un->sun_path + 1, plen - 1));
            else
                x.path = resolve(tid, AT_FDCWD, std::string(un->sun_path, strnlen(un->sun_path, plen)));
            x.family = "unix";
        } else {
            return;   // AF_UNSPEC (a UDP "disconnect"), netlink, …
        }
        if (x.family == "inet") x.proto = socket_proto(tid, static_cast<int>(a[0]));
        out.push_back(std::move(x));
    }
}

volatile sig_atomic_t g_child = 0;

void forward(int sig) {
    if (g_child > 0) kill(static_cast<pid_t>(g_child), sig);
}

void write_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

constexpr size_t kMaxRecords = 50000;

} // namespace

int supervise(char** argv, int out_fd, std::string& error) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        error = std::string("socketpair: ") + std::strerror(errno);
        return -1;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        return -1;
    }
    if (pid == 0) {
        close(sv[0]);
        // Required for an unprivileged filter; already set when sandboxed.
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        const int listener = install_listener();
        if (listener < 0) {
            std::fprintf(stderr, "funes _exec: observe: seccomp listener: %s\n", std::strerror(errno));
            _exit(126);
        }
        if (!send_fd(sv[1], listener)) _exit(126);
        close(listener);
        close(sv[1]);
        execvp(argv[0], argv);
        std::fprintf(stderr, "funes _exec: %s: %s\n", argv[0], std::strerror(errno));
        _exit(127);
    }
    close(sv[1]);
    const int listener = recv_fd(sv[0]);
    close(sv[0]);
    int status = 0;
    if (listener < 0) {
        waitpid(pid, &status, 0);
        error = "the child could not install its seccomp listener";
        return -1;
    }

    g_child = pid;
    struct sigaction sa{};
    sa.sa_handler = forward;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGTERM, SIGINT, SIGHUP}) sigaction(sig, &sa, nullptr);

    seccomp_notif_sizes sizes{};
    syscall(__NR_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sizes);
    std::vector<char> req_buf(std::max<size_t>(sizes.seccomp_notif, sizeof(seccomp_notif)));
    std::vector<char> resp_buf(std::max<size_t>(sizes.seccomp_notif_resp, sizeof(seccomp_notif_resp)));
    auto* req = reinterpret_cast<seccomp_notif*>(req_buf.data());
    auto* resp = reinterpret_cast<seccomp_notif_resp*>(resp_buf.data());

    std::set<std::string> seen;
    bool child_done = false, warned = false;
    for (;;) {
        if (!child_done && waitpid(pid, &status, WNOHANG) == pid) child_done = true;
        pollfd p{listener, POLLIN, 0};
        const int rc = poll(&p, 1, child_done ? 200 : 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc == 0) {
            if (child_done) break;   // and nothing it left behind is asking
            continue;
        }
        if (!(p.revents & POLLIN)) break;   // POLLHUP: every filtered task is gone

        std::memset(req_buf.data(), 0, req_buf.size());
        if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, req) != 0) continue;   // it died first

        std::vector<Access> found;
        const int mem = open(("/proc/" + std::to_string(req->pid) + "/mem").c_str(), O_RDONLY | O_CLOEXEC);
        if (mem >= 0) {
            decode(*req, mem, found);
            close(mem);
        } else if (!warned) {
            // Without it nothing is recorded; say so once rather than hand
            // back an empty record that reads as "touched nothing".
            std::fprintf(stderr, "funes _exec: observe: cannot read /proc/%u/mem: %s\n",
                         req->pid, std::strerror(errno));
            warned = true;
        }
        // What was read belongs to this request only if it is still pending:
        // otherwise the pid may already be somebody else.
        __u64 id = req->id;
        const bool valid = ioctl(listener, SECCOMP_IOCTL_NOTIF_ID_VALID, &id) == 0;

        std::memset(resp_buf.data(), 0, resp_buf.size());
        resp->id = req->id;
        resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
        ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, resp);

        if (!valid || out_fd < 0) continue;
        for (const auto& a : found) {
            if (seen.size() >= kMaxRecords) break;
            std::string line = to_json_line(a);
            if (seen.insert(line).second) write_all(out_fd, line + "\n");
        }
    }
    close(listener);
    if (!child_done) waitpid(pid, &status, 0);
    g_child = 0;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

// ── the fold ─────────────────────────────────────────────────────────────────

namespace {

// Rights in sandbox.h's terms: write ⊇ exec ⊇ read.
int rank(const std::string& kind) {
    return kind == "write" ? 3 : kind == "exec" ? 2 : kind == "read" ? 1 : 0;
}

int granted_rank(const funes::sandbox::Policy& p, const std::string& path) {
    int best = 0;
    for (const auto& s : p.write) if (under(path, s)) best = std::max(best, 3);
    for (const auto& s : p.exec)  if (under(path, s)) best = std::max(best, 2);
    for (const auto& s : p.read)  if (under(path, s)) best = std::max(best, 1);
    return best;
}

const char* const kRedFlags[] = {
    "/.ssh/", "/.gnupg/", "/.aws/", "/.netrc", "/.password-store/", "/.local/share/keyrings/",
    "/.config/gh/", "/.docker/config.json", "/.git-credentials", "/id_rsa", "/id_ed25519",
    "funes.local", "memory.db", "/etc/shadow", "/credentials",
};

std::string substitute(const std::string& path, const FoldContext& ctx) {
    for (const auto& [name, value] : ctx.vars)
        if (!value.empty() && under(path, value))
            return "${" + name + "}" + path.substr(value.size());
    return path;
}

struct Want {
    int rank;
    std::set<std::string> evidence;
};

void add_evidence(std::set<std::string>& into, const std::set<std::string>& from) {
    for (const auto& e : from) into.insert(e);
}

std::vector<std::string> first(const std::set<std::string>& s, const FoldContext& ctx, size_t n = 3) {
    std::vector<std::string> out;
    for (const auto& e : s) {
        if (out.size() == n) { out.push_back("(+" + std::to_string(s.size() - n) + " more)"); break; }
        std::string v = substitute(e, ctx);
        if (v.size() > 100) {
            // Cut on character boundaries: a path may well be "Música/…".
            size_t head = 60, tail = v.size() - 30;
            while (head > 0 && (static_cast<unsigned char>(v[head]) & 0xC0) == 0x80) --head;
            while (tail < v.size() && (static_cast<unsigned char>(v[tail]) & 0xC0) == 0x80) ++tail;
            v = v.substr(0, head) + "…" + v.substr(tail);
        }
        out.push_back(std::move(v));
    }
    return out;
}

} // namespace

Proposal propose(const std::vector<Access>& seen, const FoldContext& ctx) {
    Proposal out;
    out.observed = seen.size();
    std::map<std::string, Want> wants;            // path → strongest right asked for
    std::map<std::string, std::set<std::string>> used_declared;   // declared entry → evidence
    std::set<int> tcp_ports, udp_ports;
    std::map<int, std::set<std::string>> port_addrs;
    std::set<std::string> notes;

    auto implicit = [&](const std::string& path, int need) {
        if (path.rfind("${WORKSPACE}", 0) == 0) return true;
        for (const auto& pre : ctx.implicit_prefixes)
            if (path.compare(0, pre.size(), pre) == 0) return true;
        return granted_rank(ctx.implicit, path) >= need;
    };

    for (const auto& a : seen) {
        if (a.op == "connect") {
            if (a.family == "unix") {
                notes.insert("connects to unix socket " + substitute(a.path, ctx) +
                             " — not restricted by Landlock up to ABI 6; reachable whenever its file is");
            } else if (a.proto == "udp" && a.port != 0) {
                udp_ports.insert(a.port);
            } else if (a.proto != "udp" && a.port != 0) {
                tcp_ports.insert(a.port);
                port_addrs[a.port].insert(a.addr);
                if (a.proto.empty())
                    notes.insert("port " + std::to_string(a.port) + ": socket type unknown, counted as TCP");
            }
            continue;
        }
        for (const char* flag : kRedFlags)
            if (a.path.find(flag) != std::string::npos)
                notes.insert("RED FLAG: " + a.op + " " + substitute(a.path, ctx));
        if (!ctx.workspace_root.empty() && under(a.path, ctx.workspace_root))
            notes.insert("RED FLAG: " + a.op + " in another account's workspace: " + a.path);

        std::string target = a.path;
        std::string kind;
        if (a.op == "read" || a.op == "exec") {
            if (target.rfind("${", 0) != 0 && !exists(target)) continue;   // an ENOENT probe
            kind = a.op;
        } else if (a.op == "write") {
            kind = "write";
        } else if (a.op == "create" || a.op == "remove") {
            kind = "write";
            target = fs::path(target).parent_path().string();
        } else {
            continue;
        }
        const int need = rank(kind);
        if (implicit(target, need)) continue;

        // Declared? Credit the entry that covers it.
        bool covered = false;
        for (const auto* list : {&ctx.declared.write, &ctx.declared.exec, &ctx.declared.read}) {
            const int r = list == &ctx.declared.write ? 3 : list == &ctx.declared.exec ? 2 : 1;
            if (r < need) continue;
            for (const auto& d : *list)
                if (under(target, d)) { used_declared[d].insert(a.path); covered = true; }
        }
        if (covered) continue;
        auto& w = wants[target];
        w.rank = std::max(w.rank, need);
        w.evidence.insert(a.path);
    }

    // Collapse: three or more grants directly under one directory become that
    // directory (at the strongest right among them), never one of too_broad.
    auto broad = [&](const std::string& dir) {
        if (dir.empty() || dir == "/") return true;
        return std::find(ctx.too_broad.begin(), ctx.too_broad.end(), dir) != ctx.too_broad.end();
    };
    for (bool changed = true; changed;) {
        changed = false;
        std::map<std::string, std::vector<std::string>> by_parent;
        for (const auto& [p, w] : wants) by_parent[fs::path(p).parent_path().string()].push_back(p);
        for (const auto& [dir, kids] : by_parent) {
            if (kids.size() < 3 || broad(dir) || dir.rfind("${", 0) == 0) continue;
            Want merged{0, {}};
            if (auto it = wants.find(dir); it != wants.end()) merged = it->second;
            for (const auto& k : kids) {
                merged.rank = std::max(merged.rank, wants[k].rank);
                add_evidence(merged.evidence, wants[k].evidence);
                wants.erase(k);
            }
            wants[dir] = merged;
            changed = true;
            break;   // the map changed under us; group again
        }
    }
    // Landing on $HOME, the checkout, /tmp …: what it needs is real, but the
    // grant that would cover it is not one to propose. Said instead — and
    // taken out before the next step, or it would swallow every grant under it.
    auto kind_of = [](int r) { return r == 3 ? "write" : r == 2 ? "exec" : "read"; };
    for (auto it = wants.begin(); it != wants.end();) {
        if (!broad(it->first)) { ++it; continue; }
        std::string ev;
        for (const auto& e : first(it->second.evidence, ctx)) ev += (ev.empty() ? "" : ", ") + e;
        notes.insert("needs " + std::string(kind_of(it->second.rank)) + " directly in " +
                     substitute(it->first, ctx) + " (" + ev + ") — not proposed: that grant is "
                     "too broad; move the file, or grant it alone by hand");
        it = wants.erase(it);
    }
    // A grant inside another of equal or wider right adds nothing.
    for (auto it = wants.begin(); it != wants.end();) {
        bool inside = false;
        for (const auto& [q, w] : wants)
            if (q != it->first && under(it->first, q) && w.rank >= it->second.rank) { inside = true; break; }
        if (inside) {
            for (auto& [q, w] : wants)
                if (q != it->first && under(it->first, q)) add_evidence(w.evidence, it->second.evidence);
            it = wants.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& [p, w] : wants)
        out.needed.push_back({kind_of(w.rank), substitute(p, ctx), first(w.evidence, ctx)});

    auto declared_list = [&](const std::vector<std::string>& list, const char* kind) {
        for (const auto& d : list) {
            auto it = used_declared.find(d);
            if (it != used_declared.end()) out.declared.push_back({kind, substitute(d, ctx), first(it->second, ctx)});
            else out.unused.push_back({kind, substitute(d, ctx), {}});
        }
    };
    declared_list(ctx.declared.read, "read");
    declared_list(ctx.declared.exec, "exec");
    declared_list(ctx.declared.write, "write");

    for (int port : tcp_ports) {
        std::set<std::string> ev = port_addrs[port];
        const bool was_declared = std::find(ctx.declared.connect.begin(), ctx.declared.connect.end(),
                                            port) != ctx.declared.connect.end();
        (was_declared ? out.declared : out.needed).push_back({"connect", std::to_string(port), first(ev, ctx)});
    }
    for (int port : ctx.declared.connect)
        if (!tcp_ports.count(port)) out.unused.push_back({"connect", std::to_string(port), {}});

    // A UDP connect() to a port the program also reached over TCP is glibc's
    // getaddrinfo ordering its candidate addresses (RFC 6724): it connects a
    // datagram socket to learn the source address and sends nothing.
    for (int port : udp_ports) {
        const std::string why = tcp_ports.count(port) ? "getaddrinfo sorting addresses, no packet sent"
                              : port == 53            ? "DNS"
                                                      : "a real datagram";
        notes.insert("UDP to port " + std::to_string(port) + " (" + why +
                     ") — Landlock does not restrict UDP");
    }

    out.notes.assign(notes.begin(), notes.end());
    return out;
}

std::string render(const Proposal& p, const std::string& owner) {
    std::ostringstream o;
    o << "# Proposed access for " << owner << ", from " << p.observed << " observed accesses.\n"
      << "# This is what it did while watched, not what it should do: read it before committing.\n";
    std::map<std::string, std::vector<std::string>> block;
    for (const auto* list : {&p.declared, &p.needed})
        for (const auto& g : *list) block[g.kind].push_back(g.value);
    o << "access:\n";
    if (block.empty()) o << "  {}   # nothing beyond the workspace and the system\n";
    for (const char* kind : {"read", "write", "exec"}) {
        auto it = block.find(kind);
        if (it == block.end()) continue;
        o << "  " << kind << ":\n";
        // Quoted: an unquoted ${VAR} at the start of a flow item is YAML for
        // a mapping (7.6's agent-YAML bug), and a block item may as well match.
        for (const auto& v : it->second) o << "    - \"" << v << "\"\n";
    }
    if (auto it = block.find("connect"); it != block.end()) {
        o << "  connect: [";
        for (size_t i = 0; i < it->second.size(); ++i) o << (i ? ", " : "") << it->second[i];
        o << "]\n";
    }
    auto section = [&](const char* title, const std::vector<Grant>& gs) {
        if (gs.empty()) return;
        o << "# " << title << "\n";
        for (const auto& g : gs) {
            o << "#   " << g.kind << " " << g.value;
            if (!g.evidence.empty()) {
                o << "   <- ";
                for (size_t i = 0; i < g.evidence.size(); ++i) o << (i ? ", " : "") << g.evidence[i];
            }
            o << "\n";
        }
    };
    section("new (not granted today):", p.needed);
    section("declared and used:", p.declared);
    section("declared, never used while watched (candidates to drop):", p.unused);
    if (!p.notes.empty()) {
        o << "# notes:\n";
        for (const auto& n : p.notes) o << "#   " << n << "\n";
    }
    return o.str();
}

std::string log_path(const std::string& owner, const std::string& workspace_root) {
    std::string dir = funes::env("FUNES_LEARN_DIR");
    if (dir.empty()) dir = (fs::path(workspace_root).parent_path() / "learn").string();
    std::string safe;
    for (char ch : owner)
        safe += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') ? ch : '_';
    return (fs::path(dir) / (safe + ".jsonl")).string();
}

size_t append_log(const std::string& path, const std::vector<Access>& seen) {
    constexpr uintmax_t kCap = 4u << 20;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (fs::exists(path, ec) && fs::file_size(path, ec) >= kCap) return 0;
    std::set<std::string> have;
    {
        std::ifstream in(path);
        for (std::string line; std::getline(in, line);) have.insert(line);
    }
    std::ofstream out(path, std::ios::app);
    size_t added = 0;
    for (const auto& a : seen) {
        const std::string line = to_json_line(a);
        if (have.insert(line).second) { out << line << "\n"; ++added; }
    }
    return added;
}

FoldContext context_for(const std::string& own_dir, const std::string& workspace_root,
                        const funes::sandbox::Policy& declared) {
    FoldContext c;
    c.implicit = funes::sandbox::base();
    if (!own_dir.empty()) c.implicit.exec.push_back(own_dir);
    c.implicit_prefixes.push_back((fs::temp_directory_path() / "funes-exec-").string());
    c.declared = declared;
    std::error_code ec;
    if (!workspace_root.empty()) {
        c.workspace_root = fs::weakly_canonical(workspace_root, ec).string();
        if (ec) c.workspace_root = workspace_root;
    }
    const std::string root = funes::env("FUNES_ROOT");
    const char* home = std::getenv("HOME");
    if (!root.empty()) c.vars.emplace_back("FUNES_ROOT", fs::weakly_canonical(root, ec).string());
    if (home && *home) c.vars.emplace_back("HOME", fs::weakly_canonical(home, ec).string());
    std::sort(c.vars.begin(), c.vars.end(),
              [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
    c.too_broad = {"/", "/home", "/tmp", "/var", "/var/tmp", "/var/lib", "/usr", "/etc", "/opt",
                   "/srv", "/mnt", "/media", "/run", "/run/user", "/dev"};
    for (const auto& [name, value] : c.vars) {
        c.too_broad.push_back(value);
        if (name == "FUNES_ROOT") c.too_broad.push_back(value + "/data");
        if (name == "HOME") {
            for (const char* sub : {"/.config", "/.cache", "/.local", "/.local/share", "/Documents"})
                c.too_broad.push_back(value + sub);
        }
    }
    if (!c.workspace_root.empty()) c.too_broad.push_back(c.workspace_root);
    c.too_broad.push_back("/run/user/" + std::to_string(getuid()));
    return c;
}

} // namespace funes::observe
