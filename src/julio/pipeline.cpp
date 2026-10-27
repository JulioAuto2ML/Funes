// =============================================================================
// src/core/pipeline.cpp — YAML parsing for Pipeline
// =============================================================================
// See pipelines/voc.yaml for the schema by example. Validation is stricter
// than Publication's on purpose: a publication with a missing field falls back
// to the behaviour the newsletter already had, while a pipeline with a missing
// field has no earlier behaviour to fall back to — it just writes files
// somewhere nobody reads them.

#include "pipeline.h"
#include "yaml_json.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <set>
#include <yaml-cpp/yaml.h>

namespace funes {
namespace {

std::string str_or(const YAML::Node& node, const std::string& fallback) {
    if (!node || !node.IsScalar()) return fallback;
    return node.as<std::string>();
}

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

// A stage directory is joined onto the pipeline workspace, so it may not climb
// out of it or start from the root. fs_guard would refuse the resulting write
// regardless; this turns a per-call runtime refusal into one load-time error
// that names the file the mistake is actually in.
bool dir_escapes(const std::string& dir) {
    if (dir.empty()) return true;
    if (dir.front() == '/') return true;
    std::filesystem::path p(dir);
    for (const auto& part : p)
        if (part == "..") return true;
    return false;
}

} // namespace

std::string safe_slug(const std::string& raw) {
    std::string out;
    bool pending_sep = false;
    for (unsigned char c : raw) {
        if (std::isalnum(c)) {
            if (pending_sep && !out.empty()) out += '-';
            pending_sep = false;
            out += static_cast<char>(std::tolower(c));
        } else if (c < 128) {
            // Any ASCII punctuation or space is a separator, collapsed. Note
            // that '/' and '.' land here too, which is why a slug cannot be a
            // path: "../../etc/passwd" comes back as "etc-passwd".
            pending_sep = true;
        }
        // Non-ASCII bytes are dropped: the alphabet is the guarantee.
        if (out.size() >= 64) break;
    }
    if (out.size() > 64) out.resize(64);
    return out;
}

std::string safe_date(const std::string& raw) {
    if (raw.size() != 10) return {};
    for (size_t i = 0; i < raw.size(); ++i) {
        const bool want_dash = (i == 4 || i == 7);
        if (want_dash  && raw[i] != '-') return {};
        if (!want_dash && !std::isdigit(static_cast<unsigned char>(raw[i]))) return {};
    }
    return raw;
}

std::string today_iso() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string stage_relpath(const PipelineStage& stage, const std::string& date,
                          const std::string& slug) {
    const std::string d = safe_date(date).empty() ? today_iso() : date;
    const std::string s = safe_slug(slug);

    std::string name = stage.filename;
    for (const auto& [token, value] : {std::pair<std::string, std::string>{"{date}", d},
                                       std::pair<std::string, std::string>{"{slug}", s}}) {
        size_t pos = 0;
        while ((pos = name.find(token, pos)) != std::string::npos) {
            name.replace(pos, token.size(), value);
            pos += value.size();
        }
    }
    return stage.dir.empty() ? name : stage.dir + "/" + name;
}

const PipelineStage* Pipeline::stage(const std::string& name) const {
    for (const auto& s : stages)
        if (s.name == name) return &s;
    return nullptr;
}

bool Pipeline::exists(const std::string& dir, const std::string& id) {
    if (safe_id(id).empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(config_path(dir, id), ec);
}

std::vector<std::string> Pipeline::list(const std::string& dir) {
    std::vector<std::string> ids;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".yaml") continue;
        ids.push_back(entry.path().stem().string());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::string Pipeline::load(const std::string& dir, const std::string& id,
                           Pipeline& out) {
    if (safe_id(id).empty())
        return "pipeline id must contain at least one letter or digit";
    const std::filesystem::path path = config_path(dir, id);

    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const std::exception& e) {
        return "could not read " + path.string() + ": " + e.what();
    }
    if (!root.IsMap())
        return path.string() + " is not a YAML mapping";

    Pipeline p;
    p.id          = str_or(root["id"], safe_id(id));
    p.description = str_or(root["description"], "");
    p.workspace   = str_or(root["workspace"], "data/" + p.id);

    if (dir_escapes(p.workspace))
        return path.filename().string() + ": workspace must be a relative path "
               "inside the user workspace (got \"" + p.workspace + "\")";

    const YAML::Node stages = root["stages"];
    if (!stages || !stages.IsSequence() || stages.size() == 0)
        return path.filename().string() + ": needs a non-empty `stages:` sequence";

    std::set<std::string> seen;
    for (size_t i = 0; i < stages.size(); ++i) {
        const YAML::Node& node = stages[i];
        const std::string where = path.filename().string() + ": stage " +
                                  std::to_string(i + 1);
        if (!node.IsMap()) return where + " is not a mapping";

        PipelineStage s;
        s.name = str_or(node["name"], "");
        if (s.name.empty()) return where + " has no `name`";
        if (!seen.insert(s.name).second)
            return path.filename().string() + ": two stages are both named \"" +
                   s.name + "\"";

        s.dir         = str_or(node["dir"], s.name);
        s.filename    = str_or(node["filename"], s.filename);
        s.format      = str_or(node["format"], "json");
        s.description = str_or(node["description"], "");

        if (dir_escapes(s.dir))
            return where + " (\"" + s.name + "\"): `dir` must stay inside the "
                   "pipeline workspace (got \"" + s.dir + "\")";
        if (s.filename.find("{slug}") == std::string::npos &&
            s.filename.find("{date}") == std::string::npos)
            return where + " (\"" + s.name + "\"): `filename` must contain {slug} "
                   "or {date}, or every entry overwrites the last one";
        if (s.format != "json" && s.format != "text")
            return where + " (\"" + s.name + "\"): `format` must be json or text";

        if (const YAML::Node schema = node["schema"]) {
            if (s.format != "json")
                return where + " (\"" + s.name + "\"): a `schema` only applies to "
                       "a json stage";
            s.schema = yaml_to_json(schema);
        }
        p.stages.push_back(std::move(s));
    }

    out = std::move(p);
    return {};
}

} // namespace funes
