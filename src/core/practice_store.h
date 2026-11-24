// =============================================================================
// src/core/practice_store.h — a person's practice files
// =============================================================================
// <workspace>/<user>/practice/: profile.json (the range), sessions.jsonl (one
// line of scores per session), plan.json (the coach's plan for today). Files
// rather than tables so the coach and the person can both read them; but a
// model can write_file there too, so every read validates again and a broken
// file reads as absent, never as an error a page or a run would stop on.
// Used by /api/practice/* (practice_api.h) and the coach's tools
// (tools/practice_tools.cpp), so both resolve the same directory.
// =============================================================================

#pragma once
#include "practice.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace funes::practice {

std::filesystem::path user_dir(const std::string& workspace_root, int64_t user_id);
std::string now_iso();

std::optional<Profile> load_profile(const std::filesystem::path& dir);
// The last `max` sessions, oldest first; unparseable lines are skipped.
std::vector<nlohmann::json> load_sessions(const std::filesystem::path& dir, size_t max = 2000);
std::optional<nlohmann::json> load_plan(const std::filesystem::path& dir);

bool write_json(const std::filesystem::path& path, const nlohmann::json& j);   // atomic
bool append_session(const std::filesystem::path& dir, const nlohmann::json& j);

} // namespace funes::practice
