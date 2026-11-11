// =============================================================================
// tests/test_local.cpp — OS plan phase 3 (7.8): config off the environment,
// uid_users, and who funes.sock refuses
// =============================================================================

#include "funes_config.h"
#include "local_peer.h"
#include "tools/process_runner.h"
#include "users.h"
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

const fs::path kBase = fs::temp_directory_path() / "funes_local_test";

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

int test_config_is_not_environment() {
    ::setenv("FUNES_T_SHELL", "from-shell", 1);
    write(kBase / "cred" / "FUNES_T_CRED", "cred-value\n");
    write(kBase / "cred" / "funes.local", "FUNES_T_BOTH=from-credential\n");
    write(kBase / "funes.local", "FUNES_T_SECRET=s3cret\nFUNES_T_SHELL=from-file\n"
                                 "FUNES_T_BOTH=from-file\nFUNES_T_PASS=passed\n"
                                 "FUNES_CHILD_ENV=FUNES_T_PASS\n");
    funes::load_credentials((kBase / "cred").c_str());
    funes::load_config_file((kBase / "funes.local").string());

    CHECK(funes::env("FUNES_T_SECRET") == "s3cret");
    CHECK(std::getenv("FUNES_T_SECRET") == nullptr);           // the point
    CHECK(funes::env("FUNES_T_SHELL") == "from-shell");         // the environment wins
    CHECK(funes::env("FUNES_T_CRED") == "cred-value");          // a named credential
    CHECK(funes::env("FUNES_T_BOTH") == "from-credential");     // credentials before funes.local
    CHECK(funes::env_int("FUNES_T_MISSING", 7) == 7);

    // A child gets what FUNES_CHILD_ENV names, from the file, and nothing else.
    const auto child = funes::proc::child_environment();
    CHECK(has(child, "FUNES_T_PASS=passed"));
    for (const auto& e : child) CHECK(e.rfind("FUNES_T_SECRET", 0) != 0);
    std::cout << "  ok: funes.local and credentials are config, not environment\n";
    return 0;
}

int test_uid_mapping() {
    const fs::path db = kBase / "users.db";
    UserStore users(db.string());
    const int64_t admin = users.create_user("julio", "pw-pw-pw-pw", "", UserStore::ROLE_ADMIN);
    const int64_t member = users.create_user("daniela", "pw-pw-pw-pw", "", UserStore::ROLE_MEMBER);
    const int64_t machine = users.ensure_system_account("machine");
    CHECK(admin > 0 && member > 0 && machine > 0);

    CHECK(!users.resolve_uid(1000));                           // unmapped = nobody
    CHECK(users.map_uid(1000, admin));
    CHECK(users.resolve_uid(1000)->username == "julio");
    CHECK(users.map_uid(1000, member));                        // remapping moves it
    CHECK(users.resolve_uid(1000)->username == "daniela");
    CHECK(!users.map_uid(1001, machine));                      // never the system account
    CHECK(!users.resolve_uid(1001));
    CHECK(!users.map_uid(1002, 9999));                         // nor nobody
    CHECK(users.delete_user(member));
    CHECK(!users.resolve_uid(1000));                           // gone with the account
    CHECK(users.map_uid(1000, admin) && users.unmap_uid(1000) && !users.resolve_uid(1000));
    std::cout << "  ok: uid_users maps people only, and unmapped means refused\n";
    return 0;
}

int test_own_children_are_refused() {
    // A real child of this process (as a script would be of the server).
    const pid_t child = fork();
    if (child == 0) { pause(); _exit(0); }
    const bool child_refused = funes::is_own_child(child, getpid(), "");
    // This process's parent is not its descendant.
    const bool parent_refused = funes::is_own_child(getppid(), getpid(), "");
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    CHECK(child_refused);
    CHECK(!parent_refused);
    CHECK(funes::is_own_child(0, getpid(), ""));               // no pid: refused

    // By cgroup, against a fixture /proc: a grandchild that was reparented
    // to init (no ancestry left) is still inside the unit's cgroup.
    const fs::path proc = kBase / "proc";
    write(proc / "10" / "cgroup", "0::/user.slice/app.slice/funes.service/server\n");
    write(proc / "10" / "stat", "10 (funes) S 1 0\n");
    write(proc / "20" / "cgroup", "0::/user.slice/app.slice/funes.service/exec-4-python3\n");
    write(proc / "20" / "stat", "20 (python3 (x)) S 1 0\n");
    write(proc / "30" / "cgroup", "0::/user.slice/app.slice/app-gnome-terminal.scope\n");
    write(proc / "30" / "stat", "30 (bash) S 1 0\n");
    write(proc / "40" / "cgroup", "0::/user.slice/app.slice/funes.service.other\n");
    write(proc / "40" / "stat", "40 (x) S 1 0\n");
    const std::string svc = funes::service_cgroup_of(10, proc.string());
    CHECK(svc == "/user.slice/app.slice/funes.service");
    CHECK(funes::is_own_child(20, 10, svc, proc.string()));
    CHECK(!funes::is_own_child(30, 10, svc, proc.string()));
    CHECK(!funes::is_own_child(40, 10, svc, proc.string()));  // a prefix is not a parent
    // Without the cgroup module the server shares its starter's cgroup; that
    // proves nothing, and a terminal's other programs are not its children.
    CHECK(funes::service_cgroup_of(30, proc.string()).empty());
    CHECK(!funes::is_own_child(30, 10, funes::service_cgroup_of(30, proc.string()), proc.string()));
    std::cout << "  ok: Funes's own children are refused, by ancestry and by cgroup\n";
    return 0;
}

int test_framing() {
    CHECK(funes::frame_local_message("hi", "", 100) == "hi");
    const std::string m = funes::frame_local_message("what broke", "a\nb\n", 100);
    CHECK(m.rfind("what broke\n\n", 0) == 0);
    CHECK(m.find("not instructions") != std::string::npos);
    CHECK(m.find("<input>\na\nb\n\n</input>") != std::string::npos);
    const std::string t = funes::frame_local_message("", std::string(50, 'x') + "TAIL", 10);
    CHECK(t.find("bytes were left out") != std::string::npos);
    CHECK(t.find("xxxxxxTAIL\n</input>") != std::string::npos);
    CHECK(t.find(std::string(11, 'x')) == std::string::npos);
    CHECK(t.rfind("What matters", 0) == 0);                     // no question: a default one
    // The cut never splits a UTF-8 character.
    const std::string u = funes::frame_local_message("q", "ñññ", 3);
    CHECK(u.find("<input>\n[the first 4 bytes were left out; this is the end]\nñ\n</input>") != std::string::npos);
    std::cout << "  ok: piped input is fenced as data, tail kept\n";
    return 0;
}

} // namespace

int main() {
    fs::remove_all(kBase);
    int failed = 0;
    failed += test_config_is_not_environment();
    failed += test_uid_mapping();
    failed += test_own_children_are_refused();
    failed += test_framing();
    fs::remove_all(kBase);
    if (failed) { std::cerr << failed << " test(s) failed\n"; return 1; }
    std::cout << "local: all tests passed\n";
    return 0;
}
