// tests/test_staging.cpp — host changes against a copy (staging.h)
//
// What a stage refuses to overlay, that a write inside it lands in the stage
// and not in the real directory, that collect() reads the upper layer as the
// changes it is (added, modified, deleted, a directory replaced), that commit()
// applies exactly those and refuses — before writing anything — when a file
// moved underneath, and that prune() keeps a waiting proposal's stage. The
// staged call through the gate and its approval are in tests/integration.sh.
//
// The namespace half forks a child that calls staging::enter() itself (the
// trampoline's job in the server); on a kernel that refuses unprivileged user
// namespaces those tests say so and are skipped.

#include "proposals.h"
#include "staging.h"
#include "sqlite3.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using funes::staging::Change;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const fs::path kBase = fs::temp_directory_path() / ("funes_test_staging_" + std::to_string(getpid()));

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

// Runs `body` in a child inside the stage, the way the trampoline would.
// Returns the child's exit status; 77 when the kernel refused the namespace.
template <class F>
int in_stage(const std::string& stage, F body) {
    const pid_t pid = fork();
    if (pid == 0) {
        const std::string err = funes::staging::enter(stage, funes::staging::roots_of(stage));
        if (!err.empty()) { std::cerr << "  (enter: " << err << ")\n"; _exit(77); }
        _exit(body() ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

bool namespaces_refused = false;

const Change* find(const std::vector<Change>& cs, const fs::path& p) {
    for (const auto& c : cs) if (c.path == p.string()) return &c;
    return nullptr;
}

int test_prepare_refuses_what_it_cannot_stage() {
    std::string err;
    const fs::path real = kBase / "real";
    fs::create_directories(real / "inner");
    put(kBase / "afile", "x");
    CHECK(funes::staging::prepare({}, err).empty() && !err.empty());
    CHECK(funes::staging::prepare({(kBase / "afile").string()}, err).empty());
    CHECK(err.find("not a directory") != std::string::npos);
    CHECK(funes::staging::prepare({(kBase / "missing").string()}, err).empty());
    CHECK(funes::staging::prepare({real.string(), (real / "inner").string()}, err).empty());
    CHECK(err.find("overlaps") != std::string::npos);
    CHECK(funes::staging::prepare({kBase.string()}, err).empty());            // holds the stages
    CHECK(err.find("staging directory") != std::string::npos);
    fs::create_directories(kBase / "a,b");
    CHECK(funes::staging::prepare({(kBase / "a,b").string()}, err).empty());  // overlayfs option syntax
    const std::string ok = funes::staging::prepare({real.string()}, err);
    CHECK(!ok.empty() && err.empty());
    CHECK(funes::staging::roots_of(ok) == std::vector<std::string>{fs::canonical(real).string()});
    funes::staging::discard(ok);
    CHECK(!fs::exists(ok));
    std::cout << "  ok: a stage is only made for separate, existing directories outside it\n";
    return 0;
}

int test_writes_land_in_the_stage_and_commit_applies_them() {
    const fs::path real = fs::canonical(kBase / "real");
    put(real / "conf.txt", "port = 80\nname = yoda\n");
    put(real / "old.txt", "remove me\n");
    put(real / "keep.txt", "untouched\n");
    put(real / "cache" / "x", "stale\n");
    std::string err;
    const std::string stage = funes::staging::prepare({real.string()}, err);
    CHECK(!stage.empty());

    const int rc = in_stage(stage, [&] {
        std::ofstream(real / "conf.txt") << "port = 8080\nname = yoda\n";
        fs::remove(real / "old.txt");
        std::ofstream(real / "new.txt") << "hello\n";
        fs::create_directories(real / "sub");
        std::ofstream(real / "sub" / "deep.txt") << "deep\n";
        fs::remove_all(real / "cache");                     // a directory emptied...
        fs::create_directories(real / "cache");             // ...and refilled: opaque
        std::ofstream(real / "cache" / "y") << "fresh\n";
        std::ifstream k(real / "keep.txt");                 // read, not changed
        return slurp(real / "conf.txt").find("8080") != std::string::npos;
    });
    if (rc == 77) {
        namespaces_refused = true;
        funes::staging::discard(stage);
        std::cout << "  skipped: this kernel refuses an unprivileged user namespace\n";
        return 0;
    }
    CHECK(rc == 0);

    // The real directory did not move.
    CHECK(slurp(real / "conf.txt") == "port = 80\nname = yoda\n");
    CHECK(fs::exists(real / "old.txt"));
    CHECK(!fs::exists(real / "new.txt"));
    CHECK(fs::exists(real / "cache" / "x"));

    const std::vector<Change> cs = funes::staging::collect(stage);
    const Change* c;
    CHECK((c = find(cs, real / "conf.txt")) && c->kind == Change::Modified);
    CHECK((c = find(cs, real / "old.txt")) && c->kind == Change::Deleted);
    CHECK((c = find(cs, real / "new.txt")) && c->kind == Change::Added);
    CHECK((c = find(cs, real / "sub")) && c->kind == Change::Added && c->type == "dir");
    CHECK((c = find(cs, real / "sub" / "deep.txt")) && c->kind == Change::Added);
    CHECK((c = find(cs, real / "cache")) && c->kind == Change::Replaced);
    CHECK(!find(cs, real / "keep.txt"));

    const std::string summary = funes::staging::summarize(cs);
    CHECK(summary.find("modified  " + (real / "conf.txt").string()) != std::string::npos);
    CHECK(summary.find("deleted   " + (real / "old.txt").string()) != std::string::npos);
    const std::string d = funes::staging::diff(stage, 100000);
    CHECK(d.find("-port = 80") != std::string::npos);
    CHECK(d.find("+port = 8080") != std::string::npos);
    CHECK(d.find("+hello") != std::string::npos);

    size_t applied = 0;
    CHECK(funes::staging::commit(stage, &applied).empty());
    CHECK(applied == cs.size());
    CHECK(slurp(real / "conf.txt") == "port = 8080\nname = yoda\n");
    CHECK(!fs::exists(real / "old.txt"));
    CHECK(slurp(real / "new.txt") == "hello\n");
    CHECK(slurp(real / "sub" / "deep.txt") == "deep\n");
    CHECK(!fs::exists(real / "cache" / "x") && slurp(real / "cache" / "y") == "fresh\n");
    CHECK(slurp(real / "keep.txt") == "untouched\n");
    funes::staging::discard(stage);
    CHECK(!fs::exists(stage));
    std::cout << "  ok: a staged write leaves the host alone, and commit applies exactly it\n";
    return 0;
}

int test_commit_refuses_when_the_host_moved() {
    if (namespaces_refused) return 0;
    const fs::path real = fs::canonical(kBase / "real");
    put(real / "conf.txt", "v1\n");
    std::string err;
    const std::string stage = funes::staging::prepare({real.string()}, err);
    CHECK(!stage.empty());
    CHECK(in_stage(stage, [&] {
        std::ofstream(real / "conf.txt") << "v2\n";
        std::ofstream(real / "other.txt") << "added\n";
        return true;
    }) == 0);
    CHECK(funes::staging::collect(stage).size() == 2);

    put(real / "conf.txt", "edited by hand meanwhile\n");
    size_t applied = 7;
    const std::string why = funes::staging::commit(stage, &applied);
    CHECK(why.find("changed since it was staged") != std::string::npos);
    CHECK(applied == 0);
    CHECK(!fs::exists(real / "other.txt"));              // nothing half-applied
    CHECK(slurp(real / "conf.txt") == "edited by hand meanwhile\n");
    funes::staging::discard(stage);
    std::cout << "  ok: a file that changed since staging stops the whole commit\n";
    return 0;
}

int test_prune_keeps_waiting_stages() {
    const fs::path real = fs::canonical(kBase / "real");
    std::string err;
    const std::string waiting = funes::staging::prepare({real.string()}, err);
    const std::string abandoned = funes::staging::prepare({real.string()}, err);
    CHECK(!waiting.empty() && !abandoned.empty());
    CHECK(funes::staging::prune({waiting}, 3600) == 0);   // both too young
    CHECK(funes::staging::prune({waiting}, 0) == 1);
    CHECK(fs::exists(waiting) && !fs::exists(abandoned));
    funes::staging::discard(waiting);
    std::cout << "  ok: prune removes old stages no proposal is waiting on\n";
    return 0;
}

int test_proposals_carry_their_stage() {
    const std::string db = (kBase / "p.db").string();
    {
        // A 7.11 table, without the column.
        sqlite3* h = nullptr;
        sqlite3_open(db.c_str(), &h);
        sqlite3_exec(h, "CREATE TABLE proposals (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, "
                        "agent TEXT NOT NULL, tool TEXT NOT NULL, args TEXT NOT NULL, session TEXT NOT NULL DEFAULT '', "
                        "origin TEXT NOT NULL DEFAULT '', effect TEXT NOT NULL DEFAULT 'external', "
                        "status TEXT NOT NULL DEFAULT 'pending', result TEXT NOT NULL DEFAULT '', "
                        "created_at INTEGER NOT NULL, decided_at INTEGER NOT NULL DEFAULT 0);"
                        "INSERT INTO proposals (user_id, agent, tool, args, created_at) VALUES (1, 'a', 'speak', '{}', 1);",
                     nullptr, nullptr, nullptr);
        sqlite3_close(h);
    }
    funes::ProposalStore store(db);
    CHECK(store.get(1, 1) && store.get(1, 1)->stage.empty());
    const int64_t id = store.create(1, "a", "run_script", {{"name", "x"}}, "s", "web", "host", "/stage-1");
    const int64_t other = store.create(1, "a", "run_script", {{"name", "x"}}, "s", "web", "host", "/stage-2");
    CHECK(store.get(1, id)->stage == "/stage-1");
    CHECK(store.live_stages() == std::set<std::string>({"/stage-1", "/stage-2"}));
    CHECK(store.decide(1, other, false) == funes::ProposalStore::Decision::Ok);
    CHECK(store.live_stages() == std::set<std::string>({"/stage-1"}));
    store.set_now_for_test(std::time(nullptr) + funes::ProposalStore::kTtlSeconds + 10);
    CHECK(store.live_stages().empty());
    std::cout << "  ok: a proposal keeps its stage, and only waiting ones count as live\n";
    return 0;
}

} // namespace

int main() {
    fs::create_directories(kBase / "stages");
    funes::staging::set_root((kBase / "stages").string());
    int rc = 0;
    rc |= test_prepare_refuses_what_it_cannot_stage();
    rc |= test_writes_land_in_the_stage_and_commit_applies_them();
    rc |= test_commit_refuses_when_the_host_moved();
    rc |= test_prune_keeps_waiting_stages();
    rc |= test_proposals_carry_their_stage();
    funes::staging::discard(kBase.string());
    if (rc == 0) std::cout << "staging: all tests passed\n";
    return rc;
}
