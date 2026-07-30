// =============================================================================
// src/core/answer_schema.cpp
// =============================================================================

#include "answer_schema.h"
#include <algorithm>

using nlohmann::json;

namespace funes {

namespace {

// ── extraction ────────────────────────────────────────────────────────────────

bool try_parse(const std::string& s, json& out) {
    json parsed = json::parse(s, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) return false;
    out = std::move(parsed);
    return true;
}

// The body of the first ``` fence, with an optional language tag stripped.
bool fenced_block(const std::string& text, std::string& out) {
    const size_t open = text.find("```");
    if (open == std::string::npos) return false;
    size_t start = text.find('\n', open);
    if (start == std::string::npos) return false;
    ++start;
    const size_t close = text.find("```", start);
    if (close == std::string::npos) return false;
    out = text.substr(start, close - start);
    return true;
}

// Last resort: the widest {...} or [...] span in the text. Catches the very
// common "Here is the result: {...}. Let me know if…" shape.
bool widest_span(const std::string& text, std::string& out) {
    const size_t obj_start = text.find('{');
    const size_t arr_start = text.find('[');
    const bool is_obj = obj_start != std::string::npos &&
                        (arr_start == std::string::npos || obj_start < arr_start);
    const size_t start = is_obj ? obj_start : arr_start;
    if (start == std::string::npos) return false;
    const size_t end = text.rfind(is_obj ? '}' : ']');
    if (end == std::string::npos || end < start) return false;
    out = text.substr(start, end - start + 1);
    return true;
}

// ── validation ────────────────────────────────────────────────────────────────

std::string type_name(const json& v) {
    if (v.is_object())         return "object";
    if (v.is_array())          return "array";
    if (v.is_string())         return "string";
    if (v.is_boolean())        return "boolean";
    if (v.is_number_integer()) return "integer";
    if (v.is_number())         return "number";
    if (v.is_null())           return "null";
    return "unknown";
}

bool matches_type(const std::string& want, const json& v) {
    if (want == "object")  return v.is_object();
    if (want == "array")   return v.is_array();
    if (want == "string")  return v.is_string();
    if (want == "boolean") return v.is_boolean();
    if (want == "integer") return v.is_number_integer();
    if (want == "number")  return v.is_number();
    if (want == "null")    return v.is_null();
    return true;   // unknown type keyword: not our business to reject
}

std::string at(const std::string& path) {
    return path.empty() ? "the answer" : "`" + path + "`";
}

std::string check(const json& schema, const json& value, const std::string& path);

std::string check_type(const json& schema, const json& value, const std::string& path) {
    if (!schema.contains("type")) return "";
    const json& t = schema["type"];

    if (t.is_string()) {
        const std::string want = t.get<std::string>();
        if (!matches_type(want, value))
            return at(path) + ": expected " + want + ", got " + type_name(value);
        return "";
    }
    if (t.is_array()) {
        for (const auto& alt : t)
            if (alt.is_string() && matches_type(alt.get<std::string>(), value)) return "";
        return at(path) + ": expected one of " + t.dump() + ", got " + type_name(value);
    }
    return "";
}

std::string check_object(const json& schema, const json& value, const std::string& path) {
    if (!value.is_object()) return "";

    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& field : schema["required"]) {
            if (!field.is_string()) continue;
            const std::string name = field.get<std::string>();
            if (!value.contains(name))
                return at(path) + ": required field `" + name + "` is missing";
            if (value[name].is_null())
                return at(path) + ": required field `" + name + "` is null";
        }
    }

    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (const auto& [name, sub] : schema["properties"].items()) {
            if (!value.contains(name)) continue;   // absence is `required`'s business
            const std::string sub_path = path.empty() ? name : path + "." + name;
            std::string err = check(sub, value[name], sub_path);
            if (!err.empty()) return err;
        }
    }
    return "";
}

std::string check_array(const json& schema, const json& value, const std::string& path) {
    if (!value.is_array()) return "";

    if (schema.contains("minItems") && schema["minItems"].is_number_unsigned() &&
        value.size() < schema["minItems"].get<size_t>())
        return at(path) + ": expected at least " + schema["minItems"].dump() +
               " item(s), got " + std::to_string(value.size());
    if (schema.contains("maxItems") && schema["maxItems"].is_number_unsigned() &&
        value.size() > schema["maxItems"].get<size_t>())
        return at(path) + ": expected at most " + schema["maxItems"].dump() +
               " item(s), got " + std::to_string(value.size());

    if (schema.contains("items") && schema["items"].is_object()) {
        for (size_t i = 0; i < value.size(); ++i) {
            std::string err = check(schema["items"], value[i],
                                    path + "[" + std::to_string(i) + "]");
            if (!err.empty()) return err;
        }
    }
    return "";
}

std::string check(const json& schema, const json& value, const std::string& path) {
    if (!schema.is_object()) return "";

    std::string err = check_type(schema, value, path);
    if (!err.empty()) return err;

    if (schema.contains("enum") && schema["enum"].is_array()) {
        bool found = false;
        for (const auto& allowed : schema["enum"])
            if (allowed == value) { found = true; break; }
        if (!found)
            return at(path) + ": " + value.dump() + " is not one of " + schema["enum"].dump();
    }

    err = check_object(schema, value, path);
    if (!err.empty()) return err;
    return check_array(schema, value, path);
}

} // namespace

bool extract_answer_json(const std::string& answer, json& out) {
    if (answer.empty()) return false;
    if (try_parse(answer, out)) return true;

    std::string body;
    if (fenced_block(answer, body) && try_parse(body, out)) return true;
    if (widest_span(answer, body) && try_parse(body, out)) return true;
    return false;
}

std::string validate_answer(const json& schema, const json& value) {
    if (schema.is_null() || schema.empty()) return "";
    return check(schema, value, "");
}

std::string schema_prompt_block(const json& schema) {
    if (schema.is_null() || schema.empty()) return "";
    return "## Required answer format\n"
           "Your final answer must be a single JSON value matching this schema:\n\n"
           "```json\n" + schema.dump(2) + "\n```\n\n"
           "Answer with that JSON and nothing else — no preamble, no explanation "
           "after it. Intermediate steps and tool calls are unaffected; this "
           "applies only to the message you finish with.";
}

std::string schema_nudge(const std::string& error, const json& schema) {
    return "Stop. Your answer does not match the required format: " + error +
           ". Do not apologize or explain. Send the corrected answer now — the "
           "JSON value only, matching this schema: " + schema.dump() + ".";
}

std::string schema_failure(const std::string& error, const std::string& model_answer) {
    std::string s = "FAILED — this run did not produce a usable answer. The final "
                    "answer never matched the format this agent is required to "
                    "return: " + error + ". Treat the content below as unverified, "
                    "not as this agent's result.";
    if (!model_answer.empty())
        s += " Last attempt was: \"" + model_answer + "\"";
    return s;
}

} // namespace funes
