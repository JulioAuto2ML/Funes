// =============================================================================
// src/core/publication.cpp — YAML parsing for Publication
// =============================================================================
// See publications/ai-pulse.yaml for the schema by example. Every field is
// optional except `id`; the defaults in publication.h are what the AI newsletter
// ran on before any of this was configurable, so an empty config reproduces it.

#include "publication.h"
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace funes {
namespace {

std::vector<std::string> string_list(const YAML::Node& node) {
    std::vector<std::string> out;
    if (!node || !node.IsSequence()) return out;
    for (const auto& item : node)
        if (item.IsScalar()) out.push_back(item.as<std::string>());
    return out;
}

int int_or(const YAML::Node& node, int fallback) {
    if (!node || !node.IsScalar()) return fallback;
    try { return node.as<int>(); } catch (...) { return fallback; }
}

std::string str_or(const YAML::Node& node, const std::string& fallback) {
    if (!node || !node.IsScalar()) return fallback;
    return node.as<std::string>();
}

// Only [a-z0-9_-] survives, so an id chosen by a model can't walk out of the
// publications directory through the filename it lands in.
std::string safe_id(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw)
        if (std::isalnum(c) || c == '-' || c == '_')
            out += static_cast<char>(std::tolower(c));
    return out;
}

std::filesystem::path config_path(const std::string& dir, const std::string& id) {
    return std::filesystem::path(dir) / (safe_id(id) + ".yaml");
}

} // namespace

bool Publication::exists(const std::string& dir, const std::string& id) {
    if (safe_id(id).empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(config_path(dir, id), ec);
}

std::string Publication::load(const std::string& dir, const std::string& id,
                              Publication& out) {
    const std::filesystem::path path = config_path(dir, id);
    if (safe_id(id).empty())
        return "publication id must contain at least one letter or digit";

    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const std::exception& e) {
        return "could not read " + path.string() + ": " + e.what();
    }
    if (!root.IsMap())
        return path.string() + " is not a YAML mapping";

    Publication pub;
    pub.id         = str_or(root["id"], safe_id(id));
    pub.title      = str_or(root["title"], pub.id);
    pub.subject    = str_or(root["subject"], pub.title + " · {date}");
    pub.cta        = str_or(root["cta"], "");
    if (const auto voice = root["voice"])
        pub.voice_file = str_or(voice["prompt_file"], "");

    if (const auto sources = root["sources"]) {
        pub.queries         = string_list(sources["queries"]);
        pub.time_range      = str_or(sources["time_range"], pub.time_range);
        pub.per_query       = int_or(sources["per_query"], pub.per_query);
        pub.max_candidates  = int_or(sources["max_candidates"], pub.max_candidates);
        pub.max_age_days    = int_or(sources["max_age_days"], pub.max_age_days);
        pub.dedup_against_last_issues =
            int_or(sources["dedup_against_last_issues"], pub.dedup_against_last_issues);
        pub.exclude_domains = string_list(sources["exclude_domains"]);
    }

    if (const auto selection = root["selection"]) {
        pub.count          = int_or(selection["count"], pub.count);
        pub.min_count      = int_or(selection["min_count"], pub.min_count);
        pub.max_per_source = int_or(selection["max_per_source"], pub.max_per_source);
        pub.max_per_story  = int_or(selection["max_per_story"], pub.max_per_story);
    }

    if (const auto artifacts = root["artifacts"]) {
        for (const auto& node : artifacts) {
            if (!node.IsMap()) continue;
            Artifact a;
            a.kind          = str_or(node["kind"], "");
            a.path          = str_or(node["path"], "");
            a.html_template = str_or(node["template"], "");
            if (!a.kind.empty()) pub.artifacts.push_back(std::move(a));
        }
    }

    if (const auto channels = root["channels"]) {
        for (const auto& node : channels) {
            if (!node.IsMap()) continue;
            Channel c;
            c.kind            = str_or(node["kind"], "");
            c.recipients_file = str_or(node["recipients_file"], "");
            if (!c.kind.empty()) pub.channels.push_back(std::move(c));
        }
    }

    // A publication that selects more than it may select would fail at publish
    // time with a message about the model's choices rather than about this
    // file, so it is caught here where the mistake actually is.
    if (pub.min_count > pub.count)
        return path.filename().string() + ": selection.min_count (" +
               std::to_string(pub.min_count) + ") is above selection.count (" +
               std::to_string(pub.count) + ")";

    out = std::move(pub);
    return {};
}

std::string voice_text(const std::string& dir, const Publication& pub) {
    if (pub.voice_file.empty()) return {};
    std::ifstream in(std::filesystem::path(dir) / pub.voice_file);
    if (!in) return {};
    std::ostringstream body;
    body << in.rdbuf();
    return body.str();
}

} // namespace funes
