// =============================================================================
// tests/test_observe.cpp — OS plan phase 5: learn mode (src/core/observe.h)
// =============================================================================
// The supervisor against a real child (no trampoline, no sandbox: the test
// process is the supervisor), and the fold against fixtures. The proposal is
// parsed back through parse_access — a proposal a manifest cannot take is
// worse than none.

#include "observe.h"
#include "script_library.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace fs = std::filesystem;
using funes::observe::Access;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const fs::path kBase = fs::weakly_canonical(fs::temp_directory_path()) / "funes_observe_test";

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

bool has(const std::vector<Access>& v, const std::string& op, const std::string& path) {
    return std::any_of(v.begin(), v.end(), [&](const Access& a) { return a.op == op && a.path == path; });
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

int listen_tcp(int& port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 4) != 0) return -1;
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    return fd;
}

std::vector<Access> run(const std::string& script, int& rc) {
    const fs::path out = kBase / "observed.jsonl";
    fs::remove(out);
    std::ofstream(out).close();
    const int fd = ::open(out.c_str(), O_WRONLY | O_APPEND);
    std::vector<std::string> args = {"/bin/bash", "-c", script};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::string err;
    rc = funes::observe::supervise(argv.data(), fd, err);
    ::close(fd);
    if (rc < 0) std::cerr << "supervise: " << err << "\n";
    return funes::observe::parse_lines(slurp(out));
}

int test_supervisor_records_what_the_child_does() {
    const fs::path d = kBase / "tree";
    write(d / "in.txt", "x");
    write(d / "rel.txt", "x");
    write(d / "gone.txt", "x");
    write(d / "existing.txt", "x");
    int port = 0;
    const int lfd = listen_tcp(port);
    CHECK(lfd >= 0);

    int rc = 0;
    const auto seen = run(
        "cat " + (d / "in.txt").string() + " >/dev/null; "
        "echo y > " + (d / "new.txt").string() + "; "
        "echo y >> " + (d / "existing.txt").string() + "; "
        "cd " + d.string() + " && cat rel.txt >/dev/null; "
        "rm " + (d / "gone.txt").string() + "; "
        "mkdir " + (d / "sub").string() + "; "
        "mkdir " + d.string() + " 2>/dev/null; "                  // EEXIST: creates nothing
        // a grandchild: the filter is inherited, so it is watched too
        "/bin/sh -c 'cat " + (d / "in.txt").string() + " > " + (d / "sub" / "copy").string() + "'; "
        "exec 3<>/dev/tcp/127.0.0.1/" + std::to_string(port) + "; exec 3>&-; "
        "exec 4<>/dev/udp/127.0.0.1/53 2>/dev/null; "
        "exit 7",
        rc);
    ::close(lfd);

    CHECK(rc == 7);                                            // the exit status passes through
    CHECK(has(seen, "read", (d / "in.txt").string()));
    CHECK(has(seen, "create", (d / "new.txt").string()));      // O_CREAT of a new file
    CHECK(has(seen, "write", (d / "existing.txt").string()));  // an existing one, appended
    CHECK(has(seen, "read", (d / "rel.txt").string()));        // relative, resolved via cwd
    CHECK(has(seen, "remove", (d / "gone.txt").string()));
    CHECK(has(seen, "create", (d / "sub").string()));
    CHECK(!has(seen, "create", d.string()));
    CHECK(has(seen, "create", (d / "sub" / "copy").string())); // from the grandchild
    CHECK(std::any_of(seen.begin(), seen.end(), [](const Access& a) {
        return a.op == "exec" && fs::path(a.path).filename() == "cat"; }));
    CHECK(std::any_of(seen.begin(), seen.end(), [port](const Access& a) {
        return a.op == "connect" && a.port == port && a.proto == "tcp" && a.addr == "127.0.0.1"; }));
    CHECK(std::any_of(seen.begin(), seen.end(), [](const Access& a) {
        return a.op == "connect" && a.port == 53 && a.proto == "udp"; }));
    // Nothing under /proc or /sys: the base reads both, and they are noise.
    CHECK(std::none_of(seen.begin(), seen.end(), [](const Access& a) {
        return a.path.rfind("/proc/", 0) == 0 || a.path.rfind("/sys/", 0) == 0; }));
    // Recorded once each, however often it happened.
    CHECK(std::count_if(seen.begin(), seen.end(), [&](const Access& a) {
        return a.op == "read" && a.path == (d / "in.txt").string(); }) == 1);

    int killed = 0;
    run("kill -9 $$", killed);
    CHECK(killed == 128 + 9);
    std::cout << "  ok: the supervisor sees files, directories, sockets and grandchildren\n";
    return 0;
}

funes::observe::FoldContext fold_context() {
    funes::sandbox::Policy declared;
    declared.write = {(kBase / "declared_out").string()};
    declared.read = {(kBase / "never_used").string()};
    declared.connect = {443, 8002};
    auto c = funes::observe::context_for((kBase / "lib").string(), (kBase / "ws").string(), declared);
    c.vars = {{"FUNES_ROOT", (kBase / "home" / "Funes").string()}, {"HOME", (kBase / "home").string()}};
    c.too_broad.push_back((kBase / "home").string());
    return c;
}

int test_fold() {
    const fs::path home = kBase / "home";
    for (const char* f : {"a", "b", "c"}) write(home / "Funes" / "data" / "tv" / f, "x");
    write(home / "Funes" / "data" / "one" / "x", "x");
    write(home / "Funes" / "data" / "one" / "y", "x");
    write(home / ".config" / "pulse" / "cookie", "x");
    for (const char* f : {".p", ".q", ".r"}) write(home / f, "x");
    write(home / ".ssh" / "id_ed25519", "x");
    write(kBase / "ws" / "7" / "theirs.txt", "x");

    auto r = [](const std::string& op, const fs::path& p) { Access a; a.op = op; a.path = p.string(); return a; };
    std::vector<Access> seen = {
        r("exec", "/usr/bin/python3"),                        // implicit: the system
        r("read", "/etc/hosts"),
        r("read", (kBase / "lib" / "venv" / "x.py")),          // implicit: its own directory
        r("read", "${WORKSPACE}/notes.md"),                   // implicit: the caller's workspace
        r("create", "/tmp/funes-exec-AbC123/scratch"),        // implicit: its private temp dir
        r("read", (home / "nope" / "missing.py")),            // an ENOENT probe: dropped
        r("read", (home / "Funes" / "data" / "tv" / "a")),
        r("read", (home / "Funes" / "data" / "tv" / "b")),
        r("read", (home / "Funes" / "data" / "tv" / "c")),     // three siblings → the directory
        r("read", (home / "Funes" / "data" / "one" / "x")),
        r("read", (home / "Funes" / "data" / "one" / "y")),    // two do not collapse
        r("create", (home / ".config" / "pulse" / "new")),    // a create → its directory, write
        r("read", (home / ".config" / "pulse" / "cookie")),   // inside that write grant: dropped
        r("read", (home / ".p")), r("read", (home / ".q")), r("read", (home / ".r")),  // not to $HOME
        r("read", (home / ".ssh" / "id_ed25519")),
        r("read", (kBase / "ws" / "7" / "theirs.txt")),        // another account's workspace
        r("write", (kBase / "declared_out" / "f")),            // declared, used
        r("create", (home / ".npmrc")),                        // in $HOME itself: a note
    };
    Access tcp; tcp.op = "connect"; tcp.family = "inet"; tcp.proto = "tcp"; tcp.port = 443; tcp.addr = "1.2.3.4";
    Access tcp2 = tcp; tcp2.port = 8080;
    Access udp = tcp; udp.proto = "udp"; udp.port = 53;
    Access probe = tcp; probe.proto = "udp";                 // getaddrinfo's address sort
    Access un; un.op = "connect"; un.family = "unix"; un.path = "/run/user/1000/pulse/native";
    seen.insert(seen.end(), {tcp, tcp2, udp, probe, un});

    const auto p = funes::observe::propose(seen, fold_context());
    auto find = [](const std::vector<funes::observe::Grant>& gs, const std::string& kind,
                   const std::string& value) {
        return std::any_of(gs.begin(), gs.end(),
                           [&](const auto& g) { return g.kind == kind && g.value == value; });
    };
    CHECK(find(p.needed, "read", "${FUNES_ROOT}/data/tv"));             // collapsed, substituted
    CHECK(find(p.needed, "read", "${FUNES_ROOT}/data/one/x"));
    CHECK(find(p.needed, "read", "${FUNES_ROOT}/data/one/y"));
    CHECK(!find(p.needed, "read", "${FUNES_ROOT}/data/one"));
    CHECK(find(p.needed, "write", "${HOME}/.config/pulse"));
    CHECK(!find(p.needed, "read", "${HOME}/.config/pulse/cookie"));
    CHECK(find(p.needed, "read", "${HOME}/.p"));                         // never collapsed to $HOME
    CHECK(!find(p.needed, "read", "${HOME}"));
    CHECK(find(p.needed, "connect", "8080"));
    CHECK(find(p.declared, "connect", "443"));
    CHECK(find(p.unused, "connect", "8002"));
    CHECK(find(p.declared, "write", (kBase / "declared_out").string()));
    CHECK(find(p.unused, "read", (kBase / "never_used").string()));
    for (const auto& g : p.needed) {
        CHECK(g.value.find("/usr") != 0 && g.value.find("/etc") != 0);
        CHECK(g.value.find("missing") == std::string::npos);
        CHECK(g.value.find("WORKSPACE") == std::string::npos);
        CHECK(g.value.find("funes-exec") == std::string::npos);
        CHECK(g.value != "53");                                          // UDP is a note, not a port
    }
    auto note = [&](const std::string& s) {
        return std::any_of(p.notes.begin(), p.notes.end(),
                           [&](const std::string& n) { return n.find(s) != std::string::npos; });
    };
    CHECK(note("RED FLAG: read ${HOME}/.ssh/id_ed25519"));
    CHECK(note("another account's workspace"));
    CHECK(note("UDP to port 53 (DNS)"));
    CHECK(note("UDP to port 443 (getaddrinfo"));
    CHECK(note("needs write directly in ${HOME}"));
    CHECK(!find(p.needed, "write", "${HOME}"));
    CHECK(note("unix socket /run/user/1000/pulse/native"));

    // Same input, same output.
    const auto again = funes::observe::propose(seen, fold_context());
    CHECK(funes::observe::render(again, "x") == funes::observe::render(p, "x"));
    std::cout << "  ok: the fold drops what is implicit, collapses, substitutes and flags\n";

    // The proposal is a manifest's `access:` block: parse it back.
    ::setenv("FUNES_ROOT", (kBase / "home" / "Funes").c_str(), 1);
    ::setenv("HOME", (kBase / "home").c_str(), 1);
    const YAML::Node doc = YAML::Load(funes::observe::render(p, "x"));
    funes::sandbox::Policy back;
    const std::string err = funes::parse_access(doc["access"], "proposal", back);
    CHECK(err.empty());
    CHECK(std::find(back.read.begin(), back.read.end(), (home / "Funes" / "data" / "tv").string()) != back.read.end());
    CHECK(std::find(back.write.begin(), back.write.end(), (home / ".config" / "pulse").string()) != back.write.end());
    CHECK(std::find(back.connect.begin(), back.connect.end(), 8080) != back.connect.end());
    CHECK(std::find(back.connect.begin(), back.connect.end(), 443) != back.connect.end());
    CHECK(std::find(back.read.begin(), back.read.end(), (kBase / "never_used").string()) == back.read.end());
    std::cout << "  ok: the rendered proposal parses as an access: block\n";
    return 0;
}

int test_rebase_and_log() {
    std::vector<Access> seen(2);
    seen[0].op = "read";   seen[0].path = (kBase / "ws" / "3" / "a.md").string();
    seen[1].op = "read";   seen[1].path = (kBase / "ws" / "30" / "b.md").string();
    fs::create_directories(kBase / "ws" / "3");
    funes::observe::rebase(seen, (kBase / "ws" / "3").string());
    CHECK(seen[0].path == "${WORKSPACE}/a.md");
    CHECK(seen[1].path == (kBase / "ws" / "30" / "b.md").string());   // a prefix is not a parent

    ::setenv("FUNES_LEARN_DIR", (kBase / "learn").c_str(), 1);
    const std::string log = funes::observe::log_path("tv art/../x", (kBase / "ws").string());
    CHECK(fs::path(log).parent_path() == kBase / "learn");
    CHECK(fs::path(log).filename().string().find('/') == std::string::npos);
    CHECK(funes::observe::append_log(log, seen) == 2);
    CHECK(funes::observe::append_log(log, seen) == 0);                // already there
    CHECK(funes::observe::parse_lines(slurp(log) + "{broken\n").size() == 2);
    std::cout << "  ok: workspace paths rebased, the log only grows by what is new\n";
    return 0;
}

} // namespace

int main() {
    fs::remove_all(kBase);
    fs::create_directories(kBase);
    int failed = 0;
    failed += test_supervisor_records_what_the_child_does();
    failed += test_fold();
    failed += test_rebase_and_log();
    fs::remove_all(kBase);
    if (failed) { std::cerr << failed << " test(s) failed\n"; return 1; }
    std::cout << "observe: all tests passed\n";
    return 0;
}
