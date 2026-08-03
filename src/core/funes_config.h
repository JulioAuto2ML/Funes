// =============================================================================
// src/core/funes_config.h — Funes configuration loader
// =============================================================================
//
// Loads KEY=value config files into the process environment before any getenv
// calls are made. Shell environment variables always take precedence.
//
// Load order (highest → lowest priority):
//   1. Shell environment (already set — always wins)
//   2. ./config/funes.local   (gitignored local overrides / secrets)
//   3. ./config/funes.conf    (version-controlled defaults)
//   4. ~/.funes/config        (user-level config)
//
// Usage (once at the top of main(), before any getenv calls):
//   funes::load_config();
// =============================================================================

#pragma once

#include <cstdlib>
#include <fstream>
#include <string>

namespace funes {

// Load one KEY=value config file into the environment.
// Only sets variables not already present (overwrite=0).
inline void load_config_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return;

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

        setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
    }
}

inline void load_config() {
    load_config_file("./config/funes.local");
    load_config_file("./config/funes.conf");
    if (const char* home = std::getenv("HOME"))
        load_config_file(std::string(home) + "/.funes/config");
}

// getenv with a default.
inline std::string env(const char* key, const std::string& def = "") {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : def;
}

inline int env_int(const char* key, int def) {
    const char* v = std::getenv(key);
    if (!v || !*v) return def;
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
