// src/core/probes.cpp — see probes.h
#include "probes.h"
#include <cctype>
#include <ctime>
#include <iostream>

namespace funes::probes {

namespace {

// After a notice could not be sent, wait this long before trying again: the
// runner ticks every minute, and a bridge that is down would otherwise be
// asked 60 times an hour.
constexpr int64_t kNoticeRetrySeconds = 15 * 60;

bool valid_check_name(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char c : s)
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    return true;
}

std::string clock(int64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M", &tm);
    return buf;
}

std::string duration(int64_t seconds, bool es) {
    if (seconds < 3600) return std::to_string(std::max<int64_t>(1, seconds / 60)) + " min";
    if (seconds < 2 * 86400) return std::to_string(seconds / 3600) + " h";
    return std::to_string(seconds / 86400) + (es ? " días" : " days");
}

} // namespace

std::string parse_checks(const std::string& probe, const nlohmann::json& out,
                         std::vector<CheckResult>& into) {
    if (!out.is_object() || !out.contains("checks") || !out["checks"].is_array())
        return "the output is not an object with a 'checks' array";
    std::vector<CheckResult> got;
    for (const auto& c : out["checks"]) {
        if (!c.is_object() || !c.contains("name") || !c["name"].is_string() ||
            !c.contains("ok") || !c["ok"].is_boolean() ||
            !c.contains("summary") || !c["summary"].is_string())
            return "each check needs name (string), ok (boolean) and summary (string)";
        CheckResult r;
        const std::string name = c["name"].get<std::string>();
        if (!valid_check_name(name))
            return "check name '" + name.substr(0, 64) + "' is not [A-Za-z0-9_.-]";
        r.key = probe + ":" + name;
        r.ok = c["ok"].get<bool>();
        r.summary = c["summary"].get<std::string>().substr(0, 300);
        r.severity = c.value("severity", std::string("warning"));
        if (r.severity != "warning" && r.severity != "critical")
            return "check '" + name + "': severity is warning or critical";
        if (c.contains("evidence") && c["evidence"].is_string())
            r.evidence = c["evidence"].get<std::string>();
        got.push_back(std::move(r));
    }
    into.insert(into.end(), got.begin(), got.end());
    return "";
}

bool in_quiet_hours(const std::string& spec, int hour) {
    const size_t dash = spec.find('-');
    if (spec.empty() || dash == std::string::npos) return false;
    int from = -1, to = -1;
    try {
        from = std::stoi(spec.substr(0, dash));
        to = std::stoi(spec.substr(dash + 1));
    } catch (...) {
        return false;
    }
    if (from < 0 || from > 23 || to < 0 || to > 23 || from == to) return false;
    return from < to ? (hour >= from && hour < to) : (hour >= from || hour < to);
}

std::string notice_text(const Incident& i, const std::string& locale) {
    const bool es = locale.rfind("es", 0) == 0;
    std::string evidence = i.evidence;
    if (evidence.size() > 300) evidence = evidence.substr(0, 300) + "…";
    std::string t;
    if (i.resolved_at > 0 && !i.noticed_open) {
        t = (es ? "⚠️→✅ Hubo un problema y ya se resolvió solo: " : "⚠️→✅ Something broke and recovered: ") +
            i.summary + " (" + i.key + ", " + clock(i.opened_at) + "–" + clock(i.resolved_at) + ")";
    } else if (i.resolved_at > 0) {
        t = (es ? "✅ Resuelto: " : "✅ Resolved: ") + i.summary + " (" + i.key + ", " +
            (es ? "duró " : "lasted ") + duration(i.resolved_at - i.opened_at, es) + ")";
    } else {
        t = (i.severity == "critical" ? "🚨 " : "⚠️ ") +
            std::string(es ? "Funes detectó un problema: " : "Funes found a problem: ") +
            i.summary + "\n(" + i.key + (es ? ", desde las " : ", since ") + clock(i.opened_at) + ")";
        if (!evidence.empty()) t += "\n\n" + evidence;
    }
    return t;
}

Runner::Runner(incidents::IncidentStore& store, Options opt)
    : store_(store), opt_(std::move(opt)) {}

void Runner::record(const std::string& probe, const std::vector<CheckResult>& checks,
                    int fail_after, int64_t now, Report& r) {
    std::vector<std::string> seen;
    for (const auto& c : checks) {
        seen.push_back(c.key);
        const auto t = store_.observe(c, fail_after, now);
        if (t == incidents::Transition::Opened) {
            ++r.opened;
            std::cerr << "[probe] OPEN " << c.key << ": " << c.summary << "\n";
        } else if (t == incidents::Transition::Resolved) {
            ++r.resolved;
            std::cerr << "[probe] resolved " << c.key << "\n";
        }
    }
    // Includes "<probe>:(run)": the probe ran, so it is no longer broken.
    const int gone = store_.resolve_unreported(probe + ":", seen, now);
    if (gone) {
        r.resolved += gone;
        std::cerr << "[probe] " << probe << ": " << gone << " incident(s) no longer reported, resolved\n";
    }
}

void Runner::run_failed(const std::string& probe, const std::string& why, int fail_after,
                        int64_t now, Report& r) {
    std::cerr << "[probe] " << probe << " could not run: " << why.substr(0, 300) << "\n";
    CheckResult c{probe + ":(run)", false, "warning",
                  "the check '" + probe + "' could not run, so what it watches is not being watched",
                  why};
    if (store_.observe(c, fail_after, now) == incidents::Transition::Opened) ++r.opened;
}

void Runner::send_notices(int64_t now, Report& r) {
    if (!opt_.notify || now < next_notice_try_) return;
    const bool quiet = opt_.local_hour && in_quiet_hours(opt_.quiet_hours, opt_.local_hour(now));
    for (const auto& i : store_.pending_notices()) {
        if (quiet && i.severity != "critical") continue;
        if (!opt_.notify(i)) {
            std::cerr << "[probe] could not send the notice for " << i.key
                      << "; trying again in " << kNoticeRetrySeconds / 60 << " min\n";
            next_notice_try_ = now + kNoticeRetrySeconds;
            return;
        }
        store_.mark_noticed(i.id, i.resolved_at > 0);
        ++r.noticed;
    }
}

Report Runner::tick(int64_t now) {
    Report r;
    auto due = [this, now](const std::string& name, int every) {
        auto it = last_run_.find(name);
        return it == last_run_.end() || now - it->second >= every;
    };

    std::vector<ScriptProbe> scripts;
    if (opt_.list_scripts) {
        try { scripts = opt_.list_scripts(); }
        catch (const std::exception& e) { std::cerr << "[probe] listing probes failed: " << e.what() << "\n"; }
    }
    for (const auto& p : scripts) {
        if (!due(p.name, p.every_seconds)) continue;
        last_run_[p.name] = now;
        ++r.ran;
        nlohmann::json out;
        std::string err = opt_.run_script ? opt_.run_script(p.name, out) : "no script runner";
        std::vector<CheckResult> checks;
        if (err.empty()) err = parse_checks(p.name, out, checks);
        if (!err.empty()) run_failed(p.name, err, p.fail_after, now, r);
        else record(p.name, checks, p.fail_after, now, r);
    }

    for (const auto& b : opt_.builtins) {
        if (!due(b.name, b.every_seconds)) continue;
        last_run_[b.name] = now;
        ++r.ran;
        try {
            record(b.name, b.run(), b.fail_after, now, r);
        } catch (const std::exception& e) {
            run_failed(b.name, e.what(), b.fail_after, now, r);
        }
    }

    send_notices(now, r);
    return r;
}

} // namespace funes::probes
