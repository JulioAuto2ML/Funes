// src/server/local_socket.cpp — see local_socket.h
#include "local_socket.h"
#include "api.h"
#include "funes_config.h"
#include "users.h"
#include "json.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using json = nlohmann::json;

namespace funes {

namespace {

constexpr size_t kMaxRequestBytes  = 2u << 20;
constexpr size_t kDefaultMaxInput  = 12 * 1024;   // fits an 8k-token agent with room to answer
constexpr size_t kMaxMaxInput      = 256 * 1024;
constexpr int    kMaxConnections   = 4;
// With a question on the command line, a stdin that is not a terminal may be
// an open pipe nobody writes to (plain `ssh host funes ask ...`, cron, a
// script's inherited stdin). Wait this long for its first byte or EOF, then
// go without it; `-` waits for as long as it takes.
constexpr int    kStdinGraceMs     = 3000;

std::atomic<int> g_active{0};

bool send_line(int fd, const json& j) {
    const std::string s = j.dump() + "\n";
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool read_line(int fd, std::string& out, size_t max) {
    char buf[8192];
    while (out.size() <= max) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<size_t>(n));
        const size_t nl = out.find('\n');
        if (nl != std::string::npos) { out.resize(nl); return true; }
    }
    return false;
}

void serve(int fd, FunesApi& api, UserStore& users, const std::string& own_cgroup) {
    struct Done { int fd; ~Done() { ::close(fd); --g_active; } } done{fd};

    ucred cred{};
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return;
    auto refuse = [fd](const std::string& why) {
        send_line(fd, {{"type", "error"}, {"refused", true}, {"message", why}});
    };
    if (is_own_child(cred.pid, getpid(), own_cgroup)) {
        std::cerr << "[sock] refused pid " << cred.pid << " (uid " << cred.uid
                  << "): one of Funes's own child processes\n";
        return refuse("Funes's own child processes cannot use funes.sock.");
    }
    const auto user = users.resolve_uid(cred.uid);
    if (!user) {
        std::cerr << "[sock] refused uid " << cred.uid << " (pid " << cred.pid << "): not mapped\n";
        return refuse("Local uid " + std::to_string(cred.uid) + " is not mapped to a Funes "
                      "account. An admin can run: funes uid-map " + std::to_string(cred.uid) +
                      " <username>");
    }

    timeval tv{30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string line;
    if (!read_line(fd, line, kMaxRequestBytes)) return refuse("No request (one JSON line, ≤ 2 MB).");
    json req;
    try { req = json::parse(line); } catch (const std::exception&) {}
    if (!req.is_object()) return refuse("The request must be one JSON object.");

    const std::string question = req.value("message", "");
    const std::string input    = req.value("input", "");
    size_t max_input = kDefaultMaxInput;
    if (req.contains("max_input") && req["max_input"].is_number_unsigned())
        max_input = std::min<size_t>(req["max_input"].get<size_t>(), kMaxMaxInput);
    std::string session = req.value("session", "");
    if (session.empty())
        session = "ask-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(cred.pid);

    std::atomic<bool> cancelled{false};
    auto emit = [fd, &cancelled](const std::string& type, const json& data) {
        if (cancelled.load()) return;
        json out;
        if (type == "delta")          out = {{"type", "delta"}, {"text", data.value("text", "")}};
        else if (type == "tool_call") out = {{"type", "tool_call"}, {"name", data.value("name", "")}};
        else return;
        if (!send_line(fd, out)) cancelled = true;
    };
    std::cerr << "[sock] " << user->username << " (uid " << cred.uid << ", pid " << cred.pid
              << ") ask, " << input.size() << " bytes of input\n";

    cancel_flag() = &cancelled;
    try {
        std::string error;
        const std::string answer = api.run_local(
            user->id, req.value("agent", ""), frame_local_message(question, input, max_input),
            session, input.empty() ? std::vector<std::string>{} : std::vector<std::string>{"stdin"},
            input.empty() ? Persist::Full : Persist::TurnsOnly, emit, error);
        if (!error.empty()) send_line(fd, {{"type", "error"}, {"message", error}});
        else                send_line(fd, {{"type", "done"}, {"text", answer}, {"session", session}});
    } catch (const std::exception& e) {
        send_line(fd, {{"type", "error"}, {"message", e.what()}});
    }
    cancel_flag() = nullptr;
}

} // namespace

bool start_local_socket(const std::string& path, FunesApi& api, UserStore& users) {
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof addr.sun_path) {
        std::cerr << "[sock] no socket: path empty or too long\n";
        return false;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);

    // A socket file left by a previous run answers nothing; one that answers
    // is another server, which keeps it.
    struct stat st{};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            std::cerr << "[sock] " << path << " exists and is not a socket — not touching it\n";
            ::close(fd);
            return false;
        }
        const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const bool live = ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
        ::close(probe);
        if (live) {
            std::cerr << "[sock] " << path << " is served by another process — no socket here\n";
            ::close(fd);
            return false;
        }
        ::unlink(path.c_str());
    }
    // 0600 from the start: umask during bind, so there is no moment where
    // another uid could connect.
    const mode_t old = ::umask(0177);
    const int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    ::umask(old);
    if (rc != 0 || ::listen(fd, 8) != 0) {
        std::cerr << "[sock] cannot listen on " << path << ": " << std::strerror(errno) << "\n";
        ::close(fd);
        return false;
    }

    const std::string own = service_cgroup_of(getpid());
    std::thread([fd, &api, &users, own] {
        for (;;) {
            const int c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) {
                if (errno == EINTR) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (g_active.fetch_add(1) >= kMaxConnections) {
                --g_active;
                send_line(c, {{"type", "error"}, {"message", "Funes is busy (too many local requests); try again."}});
                ::close(c);
                continue;
            }
            std::thread(serve, c, std::ref(api), std::ref(users), own).detach();
        }
    }).detach();
    return true;
}

// ── the client ───────────────────────────────────────────────────────────────

int ask_main(int argc, char** argv) {
    std::string agent, session, question;
    bool verbose = false, wait_stdin = false;
    long max_input = -1;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--agent" && i + 1 < argc)          agent = argv[++i];
        else if (a == "--session" && i + 1 < argc)   session = argv[++i];
        else if (a.rfind("--max-input=", 0) == 0)    max_input = std::atol(a.c_str() + 12);
        else if (a == "-v" || a == "--verbose")      verbose = true;
        else if (a == "-")                           wait_stdin = true;
        else if (a == "-h" || a == "--help") {
            std::cout << "funes ask [--agent A] [--session S] [--max-input=BYTES] [-v] [-] [question ...]\n"
                         "  Asks the local Funes server over funes.sock. Piped stdin is sent as\n"
                         "  input (data, not instructions; its last 12 KB by default).\n"
                         "  e.g.  journalctl -u nginx -n 200 | funes ask \"what went wrong\"\n"
                         "  With a question, stdin that stays silent for 3 s is skipped; `-`\n"
                         "  waits for it (a slow command:  make 2>&1 | funes ask - \"why\").\n";
            return 0;
        }
        else question += (question.empty() ? "" : " ") + a;
    }

    std::string input;
    bool read_stdin = !isatty(STDIN_FILENO);
    if (read_stdin && !question.empty() && !wait_stdin) {
        pollfd p{STDIN_FILENO, POLLIN, 0};
        int rc;
        while ((rc = ::poll(&p, 1, kStdinGraceMs)) < 0 && errno == EINTR) {}
        if (rc == 0) {
            read_stdin = false;
            std::cerr << "funes ask: nothing on stdin after " << kStdinGraceMs / 1000
                      << " s, asking without it (`-` waits for slow input; </dev/null skips the wait)\n";
        }
    }
    if (read_stdin) {
        char buf[65536];
        ssize_t n;
        while ((n = ::read(STDIN_FILENO, buf, sizeof buf)) > 0) {
            input.append(buf, static_cast<size_t>(n));
            if (input.size() > kMaxRequestBytes - 4096) {   // keep the tail, like the server
                input.erase(0, input.size() - (kMaxRequestBytes - 4096) / 2);
            }
        }
    }
    if (question.empty() && input.empty()) {
        std::cerr << "funes ask: nothing to ask (give a question, pipe some input, or both)\n";
        return 2;
    }

    const std::string path = local_socket_path();
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof addr.sun_path) {
        std::cerr << "funes ask: no socket path (set XDG_RUNTIME_DIR or FUNES_SOCKET)\n";
        return 3;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::cerr << "funes ask: cannot reach " << path << " (" << std::strerror(errno)
                  << ") — is the Funes server running?\n";
        return 3;
    }
    json req = {{"message", question}, {"input", input}};
    if (!agent.empty())   req["agent"] = agent;
    if (!session.empty()) req["session"] = session;
    if (max_input >= 0)   req["max_input"] = static_cast<size_t>(max_input);
    if (!send_line(fd, req)) { std::cerr << "funes ask: the server closed the connection\n"; return 3; }

    std::string buf;
    bool wrote = false;
    char chunk[8192];
    for (;;) {
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            json m;
            try { m = json::parse(buf.substr(0, nl)); } catch (const std::exception&) {}
            buf.erase(0, nl + 1);
            const std::string type = m.is_object() ? m.value("type", "") : "";
            if (type == "delta") {
                std::cout << m.value("text", "") << std::flush;
                wrote = true;
            } else if (type == "tool_call") {
                if (verbose) std::cerr << "[" << m.value("name", "") << "]\n";
            } else if (type == "done") {
                if (!wrote) std::cout << m.value("text", "");
                std::cout << "\n";
                if (verbose) std::cerr << "(session " << m.value("session", "") << ")\n";
                ::close(fd);
                return 0;
            } else if (type == "error") {
                if (wrote) std::cout << "\n";
                std::cerr << "funes ask: " << m.value("message", "failed") << "\n";
                ::close(fd);
                return m.value("refused", false) ? 3 : 1;
            }
        }
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
    }
    ::close(fd);
    std::cerr << "funes ask: the connection ended without an answer\n";
    return 1;
}

} // namespace funes
