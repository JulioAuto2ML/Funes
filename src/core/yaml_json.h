// =============================================================================
// src/core/yaml_json.h — YAML node → nlohmann::json
// =============================================================================
// YAML and JSON are the same data model here, so a schema block (an agent's
// `answer_schema:`, a pipeline stage's `schema:`) is written as ordinary YAML
// and converted rather than embedded as a JSON string.
//
// yaml-cpp scalars are untyped, so numbers and booleans are recovered by
// trying the narrowest type first: `minItems: 2` has to survive as a number,
// and `"2"` would validate nothing. Note that a non-negative integer still
// arrives *signed* — answer_schema.cpp reads bounds accordingly, and there is
// a regression test for it in tests/test_answer_schema.cpp.
//
// Header-only and shared, because the two call sites are an agent config and a
// pipeline config that must agree on what `minItems: 2` means; two copies of
// this function drifting is how one of them silently stops enforcing a bound.

#pragma once
#include "json.hpp"
#include <yaml-cpp/yaml.h>

namespace funes {

inline nlohmann::json yaml_to_json(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Map: {
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& kv : node)
                obj[kv.first.as<std::string>()] = yaml_to_json(kv.second);
            return obj;
        }
        case YAML::NodeType::Sequence: {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& item : node)
                arr.push_back(yaml_to_json(item));
            return arr;
        }
        case YAML::NodeType::Scalar: {
            bool b;
            if (YAML::convert<bool>::decode(node, b)) return b;
            long long i;
            if (YAML::convert<long long>::decode(node, i)) return i;
            double d;
            if (YAML::convert<double>::decode(node, d)) return d;
            return node.as<std::string>();
        }
        case YAML::NodeType::Null:
        default:
            return nullptr;
    }
}

} // namespace funes
