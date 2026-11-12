// src/core/staging.cpp — see staging.h
#include "staging.h"
#include "json.hpp"
#include "sandbox.h"
#include "text_utils.h"
#include "tools/process_runner.h"
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sched.h>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace funes::staging {

namespace {

std::string g_root;
bool g_available = false;

const char kRootsFile[]   = "roots.json";
const char kChangesFile[] = "changes.json";

std::string layer(const std::string& stage, size_t k, const char* part) {
    return (fs::path(stage) / std::to_string(k) / part).string();
}

// overlayfs splits its options on ',' and lowerdir on ':'; a backslash
// escapes either. A path with one of them is refused rather than escaped.
bool option_safe(const std::string& p) {
    return p.find_first_of(",:\\") == std::string::npos;
}

bool within(const fs::path& inner, const fs::path& outer) {
    auto o = outer.begin(), i = inner.begin();
    for (; o != outer.end(); ++o, ++i)
        if (i == inner.end() || *i != *o) return false;
    return true;
}

bool write_file(const char* path, const std::string& text) {
    const int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    return close(fd) == 0 && ok;
}

json read_json(const fs::path& p) {
    std::ifstream in(p);
    json j = json::parse(in, nullptr, false);
    return j.is_discarded() ? json() : j;
}

// What commit() compares: an entry's type, size and modification time, as
// lstat sees them. Absent is {"exists": false}.
json fingerprint(const fs::path& p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return {{"exists", false}};
    return {{"exists", true}, {"mode", static_cast<int64_t>(st.st_mode)},
            {"size", static_cast<int64_t>(st.st_size)},
            {"mtime", static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec}};
}

std::string type_of(const fs::file_status& s) {
    if (fs::is_symlink(s))   return "symlink";
    if (fs::is_directory(s)) return "dir";
    if (fs::is_regular_file(s)) return "file";
    return "other";
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    if (fs::status(a).permissions() != fs::status(b).permissions()) return false;
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    std::istreambuf_iterator<char> xi(x), yi(y), end;
    for (; xi != end && yi != end; ++xi, ++yi)
        if (*xi != *yi) return false;
    return xi == end && yi == end;
}

bool is_whiteout(const fs::path& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == 0;
}

bool is_opaque(const fs::path& p) {
    char v[4] = {};
    const ssize_t n = lgetxattr(p.c_str(), "user.overlay.opaque", v, sizeof(v) - 1);
    return n == 1 && v[0] == 'y';
}

const char* kind_name(Change::Kind k) {
    switch (k) {
        case Change::Added:    return "added";
        case Change::Modified: return "modified";
        case Change::Deleted:  return "deleted";
        case Change::Replaced: return "replaced";
    }
    return "?";
}

// Copies `from` to `to` by way of a sibling temp name, so the real path is
// only ever the old file or the new one.
void replace_with(const fs::path& from, const fs::path& to, const std::string& type) {
    const fs::path tmp = to.parent_path() / ("." + to.filename().string() + ".funes-stage");
    fs::remove_all(tmp);
    if (type == "symlink") {
        fs::create_symlink(fs::read_symlink(from), tmp);
    } else {
        fs::copy_file(from, tmp, fs::copy_options::overwrite_existing);
        fs::permissions(tmp, fs::status(from).permissions());
    }
    std::error_code ec;
    if (fs::is_directory(fs::symlink_status(to, ec))) fs::remove_all(to);
    fs::rename(tmp, to);
}

} // namespace

void set_root(const std::string& dir) { g_root = dir; }
const std::string& root() { return g_root; }
bool available() { return g_available; }
void set_available_for_test(bool on) { g_available = on; }

std::string prepare(const std::vector<std::string>& roots, std::string& err) {
    err.clear();
    if (g_root.empty()) { err = "no staging directory"; return ""; }
    if (roots.empty())  { err = "nothing to stage"; return ""; }
    std::error_code ec;
    fs::create_directories(g_root, ec);
    const fs::path base = fs::canonical(g_root, ec);
    if (ec) { err = "staging directory: " + ec.message(); return ""; }
    if (!option_safe(base.string())) { err = "the staging directory's path has , : or \\"; return ""; }

    std::vector<fs::path> canon;
    for (const auto& r : roots) {
        const fs::path c = fs::canonical(r, ec);
        if (ec || !fs::is_directory(c)) { err = r + " is not a directory"; return ""; }
        if (!option_safe(c.string()))   { err = r + " has , : or \\ in its path"; return ""; }
        if (within(base, c) || within(c, base)) { err = r + " holds the staging directory"; return ""; }
        for (const auto& o : canon)
            if (within(c, o) || within(o, c)) { err = r + " overlaps " + o.string(); return ""; }
        canon.push_back(c);
    }

    std::string stage = (base / "stage-XXXXXX").string();
    if (!mkdtemp(stage.data())) { err = std::string("mkdtemp: ") + std::strerror(errno); return ""; }
    json list = json::array();
    for (size_t k = 0; k < canon.size(); ++k) {
        fs::create_directories(layer(stage, k, "upper"), ec);
        if (!ec) fs::create_directories(layer(stage, k, "work"), ec);
        if (ec) { err = "stage: " + ec.message(); discard(stage); return ""; }
        list.push_back(canon[k].string());
    }
    std::ofstream(fs::path(stage) / kRootsFile) << list.dump();
    return stage;
}

std::vector<std::string> roots_of(const std::string& stage) {
    std::vector<std::string> out;
    const json j = read_json(fs::path(stage) / kRootsFile);
    if (j.is_array())
        for (const auto& r : j) if (r.is_string()) out.push_back(r.get<std::string>());
    return out;
}

std::string enter(const std::string& stage, const std::vector<std::string>& roots) {
    if (!option_safe(stage)) return "stage path has , : or \\";
    const uid_t uid = getuid();
    const gid_t gid = getgid();
    // A namespace of our own, in which we hold the capabilities to mount —
    // and only there: the mapping is our own uid, so the program exec'd
    // afterwards is as unprivileged as it was.
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
        return std::string("unshare: ") + std::strerror(errno);
    if (!write_file("/proc/self/setgroups", "deny")
        || !write_file("/proc/self/uid_map", std::to_string(uid) + " " + std::to_string(uid) + " 1")
        || !write_file("/proc/self/gid_map", std::to_string(gid) + " " + std::to_string(gid) + " 1"))
        return std::string("id map: ") + std::strerror(errno);
    // Mounts made here stay here.
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0)
        return std::string("make-private: ") + std::strerror(errno);
    for (size_t k = 0; k < roots.size(); ++k) {
        if (!option_safe(roots[k])) return roots[k] + " has , : or \\";
        const std::string opts = "lowerdir=" + roots[k] + ",upperdir=" + layer(stage, k, "upper") +
                                 ",workdir=" + layer(stage, k, "work") + ",userxattr";
        if (mount("overlay", roots[k].c_str(), "overlay", 0, opts.c_str()) != 0)
            return "overlay on " + roots[k] + ": " + std::strerror(errno);
    }
    return "";
}

std::vector<Change> collect(const std::string& stage) {
    std::vector<Change> out;
    json record = json::array();
    const std::vector<std::string> roots = roots_of(stage);
    for (size_t k = 0; k < roots.size(); ++k) {
        const fs::path upper = layer(stage, k, "upper");
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(upper, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::path rel = fs::relative(it->path(), upper);
            const fs::path real = fs::path(roots[k]) / rel;
            const fs::file_status us = it->symlink_status();
            std::error_code lec;
            const fs::file_status ls = fs::symlink_status(real, lec);
            const bool lower_exists = fs::exists(ls);

            Change c;
            c.path = real.string();
            c.type = type_of(us);
            if (is_whiteout(it->path())) {
                if (!lower_exists) continue;          // made and removed again
                c.kind = Change::Deleted;
                c.type = type_of(ls);
            } else if (fs::is_directory(us)) {
                if (!lower_exists)                     c.kind = Change::Added;
                else if (is_opaque(it->path()) || !fs::is_directory(ls)) c.kind = Change::Replaced;
                else continue;                         // copied up on the way to a child
            } else if (!lower_exists) {
                c.kind = Change::Added;
            } else if (c.type == "file" && fs::is_regular_file(ls) && same_file(it->path(), real)) {
                continue;                              // copied up, not changed
            } else if (c.type == "symlink" && fs::is_symlink(ls)
                       && fs::read_symlink(it->path()) == fs::read_symlink(real)) {
                continue;
            } else {
                c.kind = Change::Modified;
            }
            record.push_back({{"root", roots[k]}, {"rel", rel.string()}, {"layer", k},
                              {"kind", kind_name(c.kind)}, {"type", c.type},
                              {"before", fingerprint(real)}});
            out.push_back(std::move(c));
        }
    }
    std::ofstream(fs::path(stage) / kChangesFile) << record.dump();
    return out;
}

std::string summarize(const std::vector<Change>& changes) {
    std::string out;
    for (const auto& c : changes) {
        std::string k = kind_name(c.kind);
        k.resize(9, ' ');
        out += "  " + k + " " + c.path + (c.type == "dir" ? "/" : "") + "\n";
    }
    return out;
}

std::string diff(const std::string& stage, size_t cap) {
    const json record = read_json(fs::path(stage) / kChangesFile);
    if (!record.is_array()) return "";
    std::string out;
    constexpr uintmax_t kMaxFile = 512 * 1024;
    for (const auto& e : record) {
        if (out.size() >= cap) break;
        if (e.value("type", "") != "file") continue;
        const std::string kind = e.value("kind", "");
        if (kind != "added" && kind != "modified") continue;
        const fs::path real  = fs::path(e.value("root", "")) / e.value("rel", "");
        const fs::path upper = fs::path(layer(stage, e.value("layer", 0), "upper")) / e.value("rel", "");
        std::error_code ec;
        if (fs::file_size(upper, ec) > kMaxFile || (kind == "modified" && fs::file_size(real, ec) > kMaxFile)) {
            out += "(" + real.string() + ": too large to show)\n";
            continue;
        }
        const funes::proc::Result r = funes::proc::run_argv(
            {"diff", "-u", "--label", "a" + real.string(), "--label", "b" + real.string(),
             kind == "added" ? "/dev/null" : real.string(), upper.string()},
            stage, 10, cap);
        // diff: 0 same, 1 different, 2 trouble (a binary file says so and exits 2... or 1)
        if (r.exit_code == 1 && funes::looks_like_text(r.output)) out += r.output;
        else out += "(" + real.string() + ": no text diff)\n";
    }
    if (out.size() > cap) {
        funes::truncate_utf8_safe(out, cap);
        out += "\n… (diff cut at " + std::to_string(cap) + " bytes)\n";
    }
    return out;
}

std::string commit(const std::string& stage, size_t* applied) {
    if (applied) *applied = 0;
    const json record = read_json(fs::path(stage) / kChangesFile);
    if (!record.is_array()) return "the stage has no record of its changes";

    // Everything is checked before anything is written.
    std::vector<fs::path> replaced;
    for (const auto& e : record) {
        const fs::path root = e.value("root", "");
        const fs::path real = root / e.value("rel", "");
        std::error_code ec;
        const fs::path parent = fs::weakly_canonical(real.parent_path(), ec);
        if (ec || !within(parent, fs::canonical(root, ec)))
            return real.string() + " is no longer inside " + root.string();
        bool under_replaced = false;
        for (const auto& r : replaced) if (within(real, r) && real != r) under_replaced = true;
        if (!under_replaced && fingerprint(real) != e["before"])
            return real.string() + " changed since it was staged";
        if (e.value("kind", "") == "replaced") replaced.push_back(real);
    }

    size_t n = 0;
    for (const auto& e : record) {
        const fs::path real  = fs::path(e.value("root", "")) / e.value("rel", "");
        const fs::path upper = fs::path(layer(stage, e.value("layer", 0), "upper")) / e.value("rel", "");
        const std::string kind = e.value("kind", ""), type = e.value("type", "");
        try {
            if (kind == "deleted") {
                fs::remove_all(real);
            } else if (type == "dir") {
                if (kind == "replaced") fs::remove_all(real);
                fs::create_directory(real);
                fs::permissions(real, fs::status(upper).permissions());
            } else if (type == "file" || type == "symlink") {
                replace_with(upper, real, type);
            } else {
                continue;   // a device or fifo: nothing a script should be making
            }
        } catch (const std::exception& ex) {
            if (applied) *applied = n;
            return "stopped after " + std::to_string(n) + " of " + std::to_string(record.size()) +
                   " changes: " + ex.what();
        }
        ++n;
    }
    if (applied) *applied = n;
    return "";
}

void discard(const std::string& stage) {
    if (stage.empty()) return;
    // overlayfs leaves work/work with mode 000; open every directory up first.
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(stage, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec))
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
    }
    ec.clear();
    // The work directory itself may be the unreadable one the iterator skipped.
    for (auto& d : fs::directory_iterator(stage, ec)) {
        std::error_code e2;
        fs::permissions(d.path() / "work" / "work", fs::perms::owner_all, fs::perm_options::add, e2);
    }
    fs::remove_all(stage, ec);
}

int prune(const std::set<std::string>& keep, int64_t min_age_seconds) {
    if (g_root.empty()) return 0;
    int removed = 0;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& d : fs::directory_iterator(g_root, ec)) {
        const std::string p = d.path().string();
        if (d.path().filename().string().rfind("stage-", 0) != 0 || keep.count(p)) continue;
        std::error_code e2;
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - d.last_write_time(e2)).count();
        if (e2 || age < min_age_seconds) continue;
        discard(p);
        ++removed;
    }
    return removed;
}

bool probe(std::string& reason) {
    g_available = false;
    if (!funes::sandbox::enabled()) { reason = "needs the sandbox"; return false; }
    if (g_root.empty()) { reason = "no staging directory"; return false; }
    // The lower side outside the staging directory, which prepare() refuses
    // to overlay.
    std::string tmpl = (fs::temp_directory_path() / "funes-stage-probe-XXXXXX").string();
    if (!mkdtemp(tmpl.data())) { reason = std::string("mkdtemp: ") + std::strerror(errno); return false; }
    const fs::path lower = tmpl;
    std::error_code ec;
    std::string stage = prepare({lower.string()}, reason);
    if (stage.empty()) return false;

    funes::sandbox::Policy p = funes::sandbox::base();
    p.write.push_back(lower.string());
    p.stage = stage;
    p.overlay = {fs::canonical(lower).string()};
    const funes::proc::Result r = funes::proc::run_argv(
        {"/bin/sh", "-c", "echo staged > '" + lower.string() + "/probe'"}, g_root, 10, 4096,
        {}, false, std::nullopt, p);
    const bool landed = fs::exists(fs::path(layer(stage, 0, "upper")) / "probe");
    const bool leaked = fs::exists(lower / "probe");
    discard(stage);
    fs::remove_all(lower, ec);
    if (r.exit_code != 0) {
        reason = r.output;
        while (!reason.empty() && std::isspace(static_cast<unsigned char>(reason.back()))) reason.pop_back();
        if (reason.empty()) reason = "exit " + std::to_string(r.exit_code);
        return false;
    }
    if (leaked)  { reason = "the write reached the real directory"; return false; }
    if (!landed) { reason = "the write did not reach the stage"; return false; }
    g_available = true;
    return true;
}

} // namespace funes::staging
