// =============================================================================
// src/core/funes_config.h — Funes configuration loader
// =============================================================================
//
// Load order (highest → lowest priority):
//   1. Shell / unit environment (already set — always wins)
//   2. systemd credentials ($CREDENTIALS_DIRECTORY, LoadCredential= in the unit)
//   3. ./config/funes.local   (gitignored local overrides / secrets)
//   4. ./config/funes.conf    (version-controlled defaults)
//
// Since 7.8 files 2–4 are read into a map private to this process, not into
// its environment. Until then every line of funes.local was setenv'd, so the
// service token, the LLM key and every API key sat in `environ`: one
// `/proc/<pid>/environ` read away, and one forgotten allowlist away from every
// child (5.0.1 defect #2, closed then by proc::child_environment's allowlist —
// a wall, but the only one). Now there is nothing in the environment to
// inherit; the allowlist is the second wall. Read config through env() /
// env_int() / config_value(), never std::getenv, or a value from a file is
// invisible.
//
// A credential named like a variable (`LoadCredential=FUNES_LLM_KEY:…`) is
// that variable's value; any other name (`funes.local`) is read as a
// KEY=value file.
//
// Usage (once at the top of main(), before any thread starts — the map is
// written here and only read afterwards):
//   funes::load_config();
// =============================================================================

#pragma once

#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <string>

namespace funes {

// What load_config() read. The first value loaded for a key wins, which is
// what makes the load order above a priority order.
inline std::map<std::string, std::string>& config_values() {
    static std::map<std::string, std::string> values;
    return values;
}

inline void parse_config_text(std::istream& f) {
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();

        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
        while (!key.empty() && (key.back()  == ' ' || key.back()  == '\t')) key.pop_back();
        if (key.empty()) continue;

        config_values().emplace(key, value);
    }
}

// Load one KEY=value config file. A key already loaded keeps its value.
inline void load_config_file(const std::string& path) {
    std::ifstream f(path);
    if (f.is_open()) parse_config_text(f);
}

inline bool is_variable_name(const std::string& s) {
    if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

// systemd's LoadCredential=: files in $CREDENTIALS_DIRECTORY, readable by
// this service alone, on a mount nothing else sees.
inline void load_credentials(const char* dir) {
    if (!dir || !*dir) return;
    DIR* d = opendir(dir);
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::ifstream f(std::string(dir) + "/" + name);
        if (!f.is_open()) continue;
        if (is_variable_name(name)) {
            std::string value((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
            config_values().emplace(name, value);
        } else {
            parse_config_text(f);
        }
    }
    closedir(d);
}

inline void load_config() {
    load_credentials(std::getenv("CREDENTIALS_DIRECTORY"));
    load_config_file("./config/funes.local");
    load_config_file("./config/funes.conf");
}

// The environment first (it always wins), then what load_config() read.
// nullptr when neither has a non-empty value.
inline const char* config_value(const std::string& key) {
    const char* v = std::getenv(key.c_str());
    if (v && *v) return v;
    const auto& m = config_values();
    auto it = m.find(key);
    return (it != m.end() && !it->second.empty()) ? it->second.c_str() : nullptr;
}

// config_value with a default.
inline std::string env(const char* key, const std::string& def = "") {
    const char* v = config_value(key);
    return v ? std::string(v) : def;
}

inline int env_int(const char* key, int def) {
    const char* v = config_value(key);
    if (!v) return def;
    int n = std::atoi(v);
    return n > 0 ? n : def;
}

// The single gate for any Funes-initiated shell execution: the execute_shell
// tool (tools/shell_tool.cpp) and scheduled shell-kind cron jobs
// (cron_runner.cpp, tools/cron_tool.cpp) all check this instead of each
// re-reading the environment variable, so the gate can't drift between them.
inline bool shell_allowed() {
    return env("FUNES_ALLOW_SHELL") == "1";
}

} // namespace funes
