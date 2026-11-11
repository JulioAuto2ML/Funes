// src/core/host_sources.cpp — see host_sources.h
#include "host_sources.h"
#include "json.hpp"
#include "text_utils.h"
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace funes::host {

namespace {

// systemd's catalog ids (/usr/lib/systemd/catalog/systemd.catalog).
constexpr const char* MSG_STARTED       = "39f53479d3a045ac8e11786248231fbf";  // start job finished
constexpr const char* MSG_STOPPED       = "9d1aaa27d60140bd96365438aad20286";  // stop job finished
constexpr const char* MSG_START_FAILED  = "be02cf6855d2428ba40df7e9d022f03d";  // start job failed
constexpr const char* MSG_UNIT_FAILED   = "d9b373ed55a64feb8242e02dbe79a49c";
constexpr const char* MSG_RESTART       = "5eb03494b6584870a536b337290809b3";  // restart scheduled
constexpr const char* MSG_OOM           = "fe6faa94e7774663a0da52717891d8ef";
constexpr const char* MSG_EXEC_FAILED   = "641257651c1b4ec9a8624d7a40a9e1e7";
constexpr const char* MSG_RESOURCES     = "ae8f7b866b0347b9af31fe1c80b127c0";
constexpr const char* MSG_SUCCEEDED     = "7ad2d189f7e94e70a38c781354912448";

std::string field(const json& e, const char* name) {
    auto it = e.find(name);
    if (it == e.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

// Transient scopes carry pids in their names (run-p256679-i4450741.scope);
// folded, so the same kind of thing is one subject.
std::string unit_subject(const json& e) {
    std::string s = field(e, "USER_UNIT");
    if (s.empty()) s = field(e, "UNIT");
    if (s.empty()) s = field(e, "_SYSTEMD_USER_UNIT");
    if (s.empty()) s = field(e, "SYSLOG_IDENTIFIER");
    if (s.empty()) s = "journal";
    // systemd-run's scopes are named by a random id (run-r<uuid>.scope) or a
    // pid (run-p<pid>-i<n>.scope): one subject for all of them.
    if (s.rfind("run-", 0) == 0 && s.size() > 6 && s.compare(s.size() - 6, 6, ".scope") == 0)
        return "run-*.scope";
    if (s.size() > 6 && s.compare(s.size() - 6, 6, ".scope") == 0) {
        std::string out;
        for (char c : s)
            if (std::isdigit(static_cast<unsigned char>(c))) {
                if (out.empty() || out.back() != '#') out += '#';
            } else out += c;
        return out;
    }
    return s;
}

int64_t parse_local_time(const std::string& date, const std::string& time) {
    std::tm tm{};
    if (sscanf(date.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) return 0;
    if (sscanf(time.c_str(), "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tm));
}

std::string strip_arch(const std::string& pkg) {
    const size_t colon = pkg.find(':');
    return colon == std::string::npos ? pkg : pkg.substr(0, colon);
}

} // namespace

// ── journal ──────────────────────────────────────────────────────────────────

std::vector<std::string> journal_argv(const std::string& cursor, const std::string& first_window) {
    std::vector<std::string> a = {"journalctl", "--user", "-o", "json", "--no-pager", "-q"};
    a.push_back(cursor.empty() ? "--since=" + first_window : "--after-cursor=" + cursor);
    // Same field = OR, `+` = OR between groups: the lifecycle messages, or
    // anything at error priority and above. The access logs and chatter that
    // make up nearly all of yoda's user journal never leave journalctl.
    for (const char* id : {MSG_STARTED, MSG_STOPPED, MSG_START_FAILED, MSG_UNIT_FAILED,
                           MSG_RESTART, MSG_OOM, MSG_EXEC_FAILED, MSG_RESOURCES, MSG_SUCCEEDED})
        a.push_back(std::string("MESSAGE_ID=") + id);
    a.push_back("+");
    for (int p = 0; p <= 3; ++p) a.push_back("PRIORITY=" + std::to_string(p));
    return a;
}

JournalBatch parse_journal(const std::string& json_lines) {
    JournalBatch out;
    std::istringstream in(json_lines);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] != '{') continue;
        json e;
        try { e = json::parse(line); } catch (const std::exception&) { continue; }
        if (!e.is_object()) continue;
        const std::string cursor = field(e, "__CURSOR");
        if (!cursor.empty()) out.cursor = cursor;

        Event ev;
        ev.source  = "journal";
        ev.subject = unit_subject(e);
        const std::string usec = field(e, "__REALTIME_TIMESTAMP");
        try { ev.ts = usec.empty() ? 0 : static_cast<int64_t>(std::stoll(usec) / 1000000); }
        catch (const std::exception&) { ev.ts = 0; }
        if (ev.ts == 0) ev.ts = static_cast<int64_t>(std::time(nullptr));

        const std::string id = field(e, "MESSAGE_ID");
        int priority = 6;
        try { priority = std::stoi(field(e, "PRIORITY")); } catch (const std::exception&) {}

        if (id == MSG_STARTED)          { ev.kind = "info";    ev.detail = "started"; }
        else if (id == MSG_STOPPED)     { ev.kind = "info";    ev.detail = "stopped"; }
        else if (id == MSG_SUCCEEDED)   { ev.kind = "info";    ev.detail = "finished successfully"; }
        else if (id == MSG_START_FAILED) {
            ev.kind = "failure";
            ev.detail = "start job failed";
            const std::string r = field(e, "JOB_RESULT");
            if (!r.empty()) ev.detail += " (" + quote_log_text(r, 32) + ")";
        } else if (id == MSG_UNIT_FAILED) {
            ev.kind = "failure";
            ev.detail = "failed";
            const std::string r = field(e, "UNIT_RESULT");
            if (!r.empty()) ev.detail += " (" + quote_log_text(r, 32) + ")";
        } else if (id == MSG_RESTART) { ev.kind = "failure"; ev.detail = "restart scheduled"; }
        else if (id == MSG_OOM)       { ev.kind = "failure"; ev.detail = "a process was killed by the OOM killer"; }
        else if (id == MSG_EXEC_FAILED) {
            ev.kind = "failure";
            ev.detail = "could not execute " + quote_log_text(field(e, "EXECUTABLE"), 120, false);
            const std::string err = field(e, "ERRNO");
            if (!err.empty() && err.size() < 5 && err.find_first_not_of("0123456789") == std::string::npos)
                ev.detail += " (errno " + err + ")";
            ev.tainted = true;
        } else if (id == MSG_RESOURCES) {
            // The cost of a unit's run — a series, like a script's.
            ev.source = "unit-run";
            ev.kind   = "sample";
            double cpu = 0, mem = 0;
            try { cpu = std::stod(field(e, "CPU_USAGE_NSEC")) / 1e9; } catch (const std::exception&) {}
            try { mem = std::stod(field(e, "MEMORY_PEAK")) / (1024.0 * 1024.0); } catch (const std::exception&) {}
            ev.value = cpu;
            std::ostringstream d;
            d.setf(std::ios::fixed);
            d.precision(1);
            d << "cpu=" << cpu << "s mem=" << mem << "MB";
            ev.detail = d.str();
        } else if (priority <= 3) {
            ev.kind    = "failure";
            ev.detail  = "error: " + quote_log_text(field(e, "MESSAGE"));
            ev.tainted = true;
        } else {
            continue;
        }
        out.events.push_back(std::move(ev));
    }
    return out;
}

// ── dpkg ─────────────────────────────────────────────────────────────────────

std::vector<Event> parse_dpkg(const std::string& text) {
    std::vector<Event> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        std::string date, time, action, pkg, from, to;
        if (!(ls >> date >> time >> action >> pkg)) continue;
        if (action != "install" && action != "upgrade" && action != "remove" && action != "purge")
            continue;
        ls >> from >> to;
        Event e;
        e.ts      = parse_local_time(date, time);
        e.source  = "dpkg";
        e.kind    = "change";
        e.subject = strip_arch(pkg);
        if (action == "upgrade")
            e.detail = from == to ? "reinstalled " + to : "upgraded " + from + " → " + to;
        else if (action == "install")
            e.detail = "installed " + to;
        else
            e.detail = (action == "purge" ? "purged " : "removed ") + from;
        if (e.ts > 0) out.push_back(std::move(e));
    }
    return out;
}

// ── /proc and /sys ───────────────────────────────────────────────────────────

std::optional<double> psi_some_avg60(const std::string& text) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("some ", 0) != 0) continue;
        const size_t p = line.find("avg60=");
        if (p == std::string::npos) return std::nullopt;
        try { return std::stod(line.substr(p + 6)); } catch (const std::exception&) {}
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<int64_t> vmstat_oom_kills(const std::string& text) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("oom_kill ", 0) != 0) continue;
        try { return std::stoll(line.substr(9)); } catch (const std::exception&) {}
    }
    return std::nullopt;
}

std::optional<std::string> read_text(const std::string& path, size_t max) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::string s(max, '\0');
    f.read(s.data(), static_cast<std::streamsize>(max));
    s.resize(static_cast<size_t>(f.gcount()));
    return s;
}

namespace {
std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}
std::optional<double> read_number(const fs::path& p) {
    auto t = read_text(p.string(), 64);
    if (!t) return std::nullopt;
    try { return std::stod(*t); } catch (const std::exception&) { return std::nullopt; }
}
} // namespace

std::vector<Reading> read_temperatures(const std::string& sys_root) {
    std::vector<Reading> out;
    std::error_code ec;
    for (const auto& hw : fs::directory_iterator(fs::path(sys_root) / "class" / "hwmon", ec)) {
        const std::string chip = trimmed(read_text((hw.path() / "name").string(), 64).value_or("hwmon"));
        std::error_code ec2;
        for (const auto& f : fs::directory_iterator(hw.path(), ec2)) {
            const std::string n = f.path().filename().string();
            if (n.rfind("temp", 0) != 0 || n.size() < 11 || n.compare(n.size() - 6, 6, "_input") != 0)
                continue;
            const std::string base = n.substr(0, n.size() - 6);   // temp1
            const auto v = read_number(f.path());
            // An unwired sensor reads 0 (or the driver's idea of absent).
            if (!v || *v <= 0) continue;
            std::string label = trimmed(read_text((hw.path() / (base + "_label")).string(), 64)
                                            .value_or(base));
            Reading r;
            r.subject = "temp:" + chip + "/" + label;
            r.value   = *v / 1000.0;
            r.limit   = read_number(hw.path() / (base + "_crit")).value_or(0) / 1000.0;
            out.push_back(r);
        }
    }
    return out;
}

std::vector<Reading> read_vram(const std::string& sys_root) {
    std::vector<Reading> out;
    std::error_code ec;
    for (const auto& card : fs::directory_iterator(fs::path(sys_root) / "class" / "drm", ec)) {
        const std::string name = card.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
        const auto used  = read_number(card.path() / "device" / "mem_info_vram_used");
        const auto total = read_number(card.path() / "device" / "mem_info_vram_total");
        if (!used || !total || *total <= 0) continue;
        out.push_back({"gpu:" + name + "/vram", *used * 100.0 / *total, 0});
    }
    return out;
}

std::vector<Reading> read_disks(const std::vector<std::string>& paths) {
    std::vector<Reading> out;
    for (const auto& p : paths) {
        struct statvfs v{};
        if (statvfs(p.c_str(), &v) != 0) continue;
        const double used  = static_cast<double>(v.f_blocks - v.f_bfree);
        const double avail = static_cast<double>(v.f_bavail);
        if (used + avail <= 0) continue;
        out.push_back({"disk:" + p, used * 100.0 / (used + avail), 0});
    }
    return out;
}

// ── processes outside systemd ────────────────────────────────────────────────

std::map<std::string, std::string> read_llama(const std::string& pid_dir,
                                              const std::string& proc_root) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(pid_dir, ec)) {
        const std::string n = f.path().filename().string();
        if (n.rfind("llama_", 0) != 0 || f.path().extension() != ".pid") continue;
        const std::string name = n.substr(6, n.size() - 6 - 4);
        const std::string pid = trimmed(read_text(f.path().string(), 32).value_or(""));
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        std::string model;
        bool alive = false;
        if (auto cmd = read_text(proc_root + "/" + pid + "/cmdline", 16384)) {
            alive = true;
            std::vector<std::string> args;
            std::string cur;
            for (char c : *cmd) {
                if (c == '\0') { args.push_back(cur); cur.clear(); } else cur += c;
            }
            for (size_t i = 0; i + 1 < args.size(); ++i)
                if (args[i] == "-m" || args[i] == "--model") model = args[i + 1];
        }
        out[name] = pid + "|" + model + "|" + (alive ? "1" : "0");
    }
    return out;
}

// ── files ────────────────────────────────────────────────────────────────────

std::map<std::string, std::string> fingerprint(const std::vector<std::string>& paths) {
    std::map<std::string, std::string> out;
    auto add = [&out](const fs::path& p) {
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        if (ec) return;
        const auto mtime = fs::last_write_time(p, ec);
        if (ec) return;
        out[p.string()] = std::to_string(mtime.time_since_epoch().count()) + "|" +
                          std::to_string(size);
    };
    for (const auto& p : paths) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (const auto& f : fs::directory_iterator(p, ec))
                if (f.is_regular_file(ec)) add(f.path());
        } else if (fs::is_regular_file(p, ec)) {
            add(p);
        }
    }
    return out;
}

// ── helpers ──────────────────────────────────────────────────────────────────

std::string quote_log_text(const std::string& text, size_t max, bool fold_digits) {
    std::string out;
    for (unsigned char c : text) {
        if (fold_digits && std::isdigit(c)) {
            if (out.empty() || out.back() != '#') out += '#';
        } else if (c < 0x20 || c == 0x7f) {
            if (out.empty() || out.back() != ' ') out += ' ';
        } else if (c == '"') {
            out += '\'';
        } else {
            out += static_cast<char>(c);
        }
    }
    if (out.size() > max) {
        funes::truncate_utf8_safe(out, max);
        out += "…";
    }
    return "\"" + out + "\"";
}

} // namespace funes::host
