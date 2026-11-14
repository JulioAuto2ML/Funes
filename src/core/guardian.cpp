// src/core/guardian.cpp — see guardian.h
#include "guardian.h"
#include "funes_config.h"
#include "json.hpp"
#include "tools/process_runner.h"
#include <cctype>
#include <iostream>
#include <mutex>

namespace funes::guardian {

namespace {

std::mutex g_mu;
std::string g_script;
std::string g_root;

Result call(const std::vector<std::string>& args) {
    std::string script, root;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        script = g_script;
        root = g_root;
    }
    if (script.empty()) return {false, 0, "the guardian is not configured"};
    std::vector<std::string> argv = {"python3", script, "--root", root};
    argv.insert(argv.end(), args.begin(), args.end());
    // What the guardian reads from the environment when it is the timer;
    // here it is passed by name, since children get an allowlist.
    std::vector<std::pair<std::string, std::string>> env;
    for (const char* key : {"FUNES_DB", "FUNES_PORT", "WHATSAPP_BRIDGE_URL", "WHATSAPP_DB_PATH"})
        if (const char* v = funes::config_value(key)) env.emplace_back(key, v);
    const auto r = funes::proc::run_argv(argv, root, 120, 1u << 20, env, true);
    const auto out = nlohmann::json::parse(r.output, nullptr, false);
    if (r.timed_out || !out.is_object()) {
        std::cerr << "[guardian] " << (args.empty() ? "" : args[0]) << " failed (exit " << r.exit_code
                  << "): " << r.stderr_output.substr(0, 300) << "\n";
        return {false, 0, "the guardian did not answer: " +
                              (r.timed_out ? std::string("timed out") : r.stderr_output.substr(0, 200))};
    }
    Result res;
    res.ok = out.value("ok", true) && !out.contains("error");
    res.id = out.value("id", int64_t{0});
    res.message = out.contains("error") ? out.value("error", std::string())
                                        : out.value("message", std::string());
    return res;
}

} // namespace

void configure(const std::string& script, const std::string& root) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_script = script;
    g_root = root;
}

bool enabled() {
    std::lock_guard<std::mutex> lock(g_mu);
    return !g_script.empty();
}

Result begin(const std::string& reason, const std::string& actor,
             const std::vector<std::string>& paths, const std::vector<std::string>& restart) {
    std::vector<std::string> args = {"begin", "--reason", reason, "--actor", actor};
    for (const auto& p : paths) { args.push_back("--path"); args.push_back(p); }
    for (const auto& u : restart) { args.push_back("--restart"); args.push_back(u); }
    Result r = call(args);
    if (r.ok && r.id <= 0) { r.ok = false; r.message = "the guardian returned no change number"; }
    std::cerr << "[guardian] begin '" << reason << "': "
              << (r.ok ? "#" + std::to_string(r.id) : "FAILED (" + r.message + ")") << "\n";
    return r;
}

Result applied(int64_t id, int confirm_after_seconds) {
    Result r = call({"applied", std::to_string(id), "--confirm-after",
                     std::to_string(confirm_after_seconds)});
    if (!r.ok) std::cerr << "[guardian] applied #" << id << " FAILED: " << r.message << "\n";
    return r;
}

Result undo(int64_t id, const std::string& by) {
    Result r = call({"undo", std::to_string(id), "--by", by});
    std::cerr << "[guardian] undo #" << id << " by " << by << ": " << (r.ok ? "done" : "refused") << "\n";
    return r;
}

std::optional<int64_t> parse_undo(const std::string& message) {
    std::string s;
    for (unsigned char c : message) s += static_cast<char>(std::tolower(c));
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '.' ||
                          s.back() == '!'))
        s.pop_back();
    size_t pos = s.find_first_not_of(" \t\r\n");
    if (pos == std::string::npos) return std::nullopt;
    bool matched = false;
    for (const char* verb : {"deshacer", "undo"}) {
        const std::string v = verb;
        if (s.compare(pos, v.size(), v) == 0) { pos += v.size(); matched = true; break; }
    }
    if (!matched || pos >= s.size() || !std::isspace(static_cast<unsigned char>(s[pos]))) return std::nullopt;
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    if (pos < s.size() && s[pos] == '#') ++pos;
    const size_t digits = pos;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
    if (pos == digits || pos != s.size() || pos - digits > 6) return std::nullopt;
    return std::stoll(s.substr(digits));
}

} // namespace funes::guardian
