// =============================================================================
// src/core/script_library.cpp — YAML parsing + argv construction for scripts
// =============================================================================
// See scriptlib/README.md for the manifest schema by example.

#include "script_library.h"
#include "answer_schema.h"
#include "yaml_json.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <yaml-cpp/yaml.h>

namespace fs = std::filesystem;

namespace funes {
namespace {

constexpr int kMaxTimeoutSeconds = 120;   // same ceiling execute_shell uses
constexpr size_t kMaxArgBytes    = 4096;  // per argument value

// ${VAR} from the server's environment. Same rule agent_config.cpp uses for
// llm_api_key and an MCP server's env: an unset variable expands to nothing,
// because a manifest may name an optional key.
std::string expand_env(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '{') {
            const size_t close = s.find('}', i + 2);
            if (close != std::string::npos) {
                const std::string var = s.substr(i + 2, close - i - 2);
                if (const char* val = std::getenv(var.c_str())) out += val;
                i = close + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

std::string str_or(const YAML::Node& node, const std::string& fallback) {
    if (!node || !node.IsScalar()) return fallback;
    return node.as<std::string>();
}

// A value that reaches a child process's argv. Control characters are refused
// rather than stripped: a script's argument parser and the model's idea of
// what it sent must agree, and a silently altered value is how they stop
// agreeing.
bool clean_value(const std::string& v, std::string& why) {
    if (v.size() > kMaxArgBytes) { why = "longer than 4096 bytes"; return false; }
    for (unsigned char c : v)
        if (c < 0x20 || c == 0x7f) { why = "contains a control character"; return false; }
    return true;
}

std::string json_scalar(const nlohmann::json& v) {
    if (v.is_string())  return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number())  {
        std::ostringstream oss;
        oss << v.get<double>();
        return oss.str();
    }
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return {};
}

} // namespace

std::string safe_script_name(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_')
            out += static_cast<char>(std::tolower(c));
        if (out.size() >= 64) break;
    }
    return out;
}

std::string ScriptSpec::usage() const {
    if (params.empty()) return "takes no arguments";
    std::ostringstream oss;
    for (size_t i = 0; i < params.size(); ++i) {
        const ScriptParam& p = params[i];
        if (i) oss << "; ";
        oss << p.name << " (" << p.type << (p.required ? ", required" : ", optional");
        if (!p.choices.empty()) {
            oss << ", one of: ";
            for (size_t j = 0; j < p.choices.size(); ++j)
                oss << (j ? "|" : "") << p.choices[j];
        }
        oss << ")";
        if (!p.description.empty()) oss << " — " << p.description;
    }
    return oss.str();
}

std::string ScriptSpec::returns() const {
    if (output_format != "json") return "text";
    std::string out = "JSON";
    if (output_schema.is_object()) {
        const auto type = output_schema.value("type", std::string());
        if (!type.empty()) out += " " + type;
        if (output_schema.contains("required") && output_schema["required"].is_array()) {
            out += " with keys: ";
            const auto& req = output_schema["required"];
            for (size_t i = 0; i < req.size(); ++i)
                if (req[i].is_string())
                    out += (i ? ", " : "") + req[i].get<std::string>();
        }
    }
    return out;
}

std::string validate_output(const ScriptSpec& spec, const std::string& stdout_text,
                            nlohmann::json& parsed) {
    if (spec.output_format != "json") return {};

    // Strict, unlike an agent's final answer: extract_answer_json is generous
    // because a model writes prose around its JSON, and a script does not get
    // that latitude — it is a program, and "print exactly one JSON value" is a
    // thing a program can do every time. Being lenient here would let a stray
    // debug line decide which of two JSON objects the next stage reads.
    std::string trimmed = stdout_text;
    const auto first = trimmed.find_first_not_of(" \t\r\n");
    const auto last  = trimmed.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "Script '" + spec.name + "' declares JSON output but printed nothing. "
               "This is a fault in the installed script, not in how it was called.";
    trimmed = trimmed.substr(first, last - first + 1);

    parsed = nlohmann::json::parse(trimmed, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded())
        return "Script '" + spec.name + "' declares JSON output but printed something "
               "that is not JSON. This is a fault in the installed script, not in how "
               "it was called — report it rather than retrying with other arguments.";

    if (spec.output_schema.is_object() && !spec.output_schema.empty()) {
        const std::string err = validate_answer(spec.output_schema, parsed);
        if (!err.empty())
            return "Script '" + spec.name + "' printed JSON that does not match the "
                   "shape its manifest declares (" + spec.returns() + "): " + err +
                   ". This is a fault in the installed script, not in how it was called.";
    }
    return {};
}

std::string load_script(const std::string& dir, const std::string& name, ScriptSpec& out) {
    const std::string id = safe_script_name(name);
    if (id.empty() || id != name)
        return "Invalid script name '" + name + "' — script names are letters, "
               "digits, '-' and '_' only.";

    const fs::path manifest = fs::path(dir) / (id + ".yaml");
    std::error_code ec;
    if (!fs::exists(manifest, ec))
        return "No script named '" + id + "' is installed.";

    YAML::Node root;
    try {
        root = YAML::LoadFile(manifest.string());
    } catch (const YAML::Exception& e) {
        return "Script manifest '" + manifest.string() + "' is not valid YAML: " + e.what();
    }

    ScriptSpec spec;
    spec.name        = id;   // the filename, never a `name:` field that could disagree with it
    spec.description = str_or(root["description"], "");
    spec.interpreter = str_or(root["interpreter"], "");

    // The executable file. A plain filename inside the library: the library is
    // the boundary, so a manifest may not point `run:` at /usr/bin/anything or
    // climb out with "..". That would make the grant meaningless — the whole
    // claim is that an allowed script is a reviewed file in one reviewed
    // directory.
    const std::string run = str_or(root["run"], "");
    if (run.empty())
        return "Script '" + id + "' has no `run:` field naming its executable file.";
    if (run.find('/') != std::string::npos || run == "." || run == "..")
        return "Script '" + id + "': `run:` must be a filename inside the script "
               "library, not a path ('" + run + "').";
    const fs::path file = fs::path(dir) / run;
    if (!fs::exists(file, ec))
        return "Script '" + id + "' names a file that is not installed: " + run;
    spec.file = fs::absolute(file, ec).string();
    if (ec) spec.file = file.string();

    if (root["timeout_seconds"]) {
        try { spec.timeout_seconds = root["timeout_seconds"].as<int>(); }
        catch (const YAML::Exception&) { /* keep the default */ }
    }
    if (spec.timeout_seconds < 1) spec.timeout_seconds = 1;
    if (spec.timeout_seconds > kMaxTimeoutSeconds) spec.timeout_seconds = kMaxTimeoutSeconds;

    if (root["params"] && root["params"].IsSequence()) {
        for (const auto& node : root["params"]) {
            ScriptParam p;
            if (node.IsScalar()) {
                p.name = node.as<std::string>();
            } else {
                p.name        = str_or(node["name"], "");
                p.description = str_or(node["description"], "");
                p.type        = str_or(node["type"], "string");
                if (node["required"]) {
                    try { p.required = node["required"].as<bool>(); }
                    catch (const YAML::Exception&) {}
                }
                if (node["choices"] && node["choices"].IsSequence())
                    for (const auto& c : node["choices"])
                        p.choices.push_back(c.as<std::string>());
            }
            if (p.name.empty())
                return "Script '" + id + "' has a parameter with no name.";
            if (p.type != "string" && p.type != "number" && p.type != "boolean")
                return "Script '" + id + "', parameter '" + p.name + "': type must be "
                       "string, number or boolean (got '" + p.type + "').";
            spec.params.push_back(std::move(p));
        }
    }

    // output: { format: json, schema: {...} }
    if (root["output"] && root["output"].IsMap()) {
        spec.output_format = str_or(root["output"]["format"], "text");
        if (spec.output_format != "text" && spec.output_format != "json")
            return "Script '" + id + "': output.format must be text or json (got '" +
                   spec.output_format + "').";
        if (root["output"]["schema"] && root["output"]["schema"].IsMap()) {
            if (spec.output_format != "json")
                return "Script '" + id + "': output.schema needs output.format: json — "
                       "a schema on text output would never be checked.";
            spec.output_schema = yaml_to_json(root["output"]["schema"]);
        }
    }

    if (root["env"] && root["env"].IsMap())
        for (const auto& kv : root["env"])
            spec.env.emplace_back(kv.first.as<std::string>(),
                                  expand_env(kv.second.as<std::string>()));

    out = std::move(spec);
    return {};
}

std::vector<std::string> list_script_names(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return names;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".yaml") continue;
        const std::string stem = entry.path().stem().string();
        if (safe_script_name(stem) == stem) names.push_back(stem);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<ScriptSpec> load_scripts(const std::string& dir,
                                     const std::vector<std::string>& allowed) {
    std::vector<ScriptSpec> out;
    for (const auto& name : allowed) {
        ScriptSpec spec;
        if (load_script(dir, name, spec).empty()) out.push_back(std::move(spec));
    }
    return out;
}

std::string build_argv(const ScriptSpec& spec, const nlohmann::json& args,
                       std::vector<std::string>& argv) {
    const nlohmann::json empty = nlohmann::json::object();
    const nlohmann::json& in = args.is_object() ? args : empty;

    // Unknown first: a model that invented a parameter has usually
    // misremembered the script, and running it with the rest of the arguments
    // would be doing something other than what was asked.
    for (auto it = in.begin(); it != in.end(); ++it) {
        const bool known = std::any_of(spec.params.begin(), spec.params.end(),
                                       [&](const ScriptParam& p) { return p.name == it.key(); });
        if (!known)
            return "Script '" + spec.name + "' has no parameter '" + it.key() +
                   "'. Accepted: " + spec.usage();
    }

    std::vector<std::string> built;
    if (!spec.interpreter.empty()) built.push_back(spec.interpreter);
    built.push_back(spec.file);

    for (const ScriptParam& p : spec.params) {
        if (!in.contains(p.name) || in.at(p.name).is_null()) {
            if (p.required)
                return "Script '" + spec.name + "' requires the parameter '" + p.name +
                       "'. Accepted: " + spec.usage();
            continue;
        }
        const nlohmann::json& v = in.at(p.name);

        if (p.type == "boolean") {
            bool on;
            if (v.is_boolean())     on = v.get<bool>();
            else if (v.is_string()) on = (v.get<std::string>() == "true");
            else return "Script '" + spec.name + "', parameter '" + p.name +
                        "': expected true or false.";
            if (on) built.push_back("--" + p.name);   // flags are presence, not "--p=true"
            continue;
        }

        if (v.is_object() || v.is_array())
            return "Script '" + spec.name + "', parameter '" + p.name +
                   "': expected a " + p.type + ", not an object or array.";

        std::string value = json_scalar(v);
        if (p.type == "number" && !v.is_number()) {
            // A local model often sends a number as a string; accept it only
            // if it really is one, rather than handing the script prose.
            try {
                size_t used = 0;
                (void)std::stod(value, &used);
                if (used != value.size()) throw std::invalid_argument("trailing");
            } catch (const std::exception&) {
                return "Script '" + spec.name + "', parameter '" + p.name +
                       "': expected a number, got '" + value + "'.";
            }
        }
        std::string why;
        if (!clean_value(value, why))
            return "Script '" + spec.name + "', parameter '" + p.name + "': value " + why + ".";
        if (!p.choices.empty() &&
            std::find(p.choices.begin(), p.choices.end(), value) == p.choices.end())
            return "Script '" + spec.name + "', parameter '" + p.name + "': '" + value +
                   "' is not one of the accepted values. " + spec.usage();

        // One token, not two: `--name=value` can never be read as an option in
        // its own right, whatever the value starts with. A separate `--name`
        // `value` pair would let a value of "--force" arrive at the script as
        // a flag it recognises.
        built.push_back("--" + p.name + "=" + value);
    }

    argv = std::move(built);
    return {};
}

} // namespace funes
