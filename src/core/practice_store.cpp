// =============================================================================
// src/core/practice_store.cpp — see practice_store.h
// =============================================================================

#include "practice_store.h"
#include "tools/fs_guard.h"
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace funes::practice {

fs::path user_dir(const std::string& workspace_root, int64_t user_id) {
    return fsguard::workspace_for(workspace_root, user_id, "") / "practice";
}

std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::optional<Profile> load_profile(const fs::path& dir) {
    const fs::path path = dir / "profile.json";
    std::ifstream in(path);
    if (!in) return std::nullopt;
    try {
        Profile p;
        if (std::string err = profile_from_json(json::parse(in), p); !err.empty()) {
            std::cerr << "[practice] " << path.string() << ": " << err << "\n";
            return std::nullopt;
        }
        return p;
    } catch (const std::exception& e) {
        std::cerr << "[practice] " << path.string() << ": " << e.what() << "\n";
        return std::nullopt;
    }
}

std::vector<json> load_sessions(const fs::path& dir, size_t max) {
    std::ifstream in(dir / "sessions.jsonl");
    std::deque<json> last;
    std::string line;
    while (std::getline(in, line)) {
        json j = json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        last.push_back(std::move(j));
        if (last.size() > max) last.pop_front();
    }
    return {last.begin(), last.end()};
}

std::optional<json> load_plan(const fs::path& dir) {
    std::ifstream in(dir / "plan.json");
    if (!in) return std::nullopt;
    json j = json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("items") || !j["items"].is_array()) return std::nullopt;
    return j;
}

bool write_json(const fs::path& path, const json& j) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << j.dump(2) << "\n";
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

bool append_session(const fs::path& dir, const json& j) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(dir / "sessions.jsonl", std::ios::app);
    if (!out) return false;
    out << j.dump() << "\n";
    return static_cast<bool>(out);
}

} // namespace funes::practice
