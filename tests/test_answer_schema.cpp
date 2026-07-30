// =============================================================================
// tests/test_answer_schema.cpp — typed final answers
// =============================================================================
// Two things are being protected here. Extraction has to be generous, because
// a local model will fence its JSON, or prefix a sentence to it, no matter how
// the prompt is worded — and rejecting an answer that is right but wrapped
// just burns the nudge budget. Validation has to be strict, because the whole
// point is that a malformed answer can't pass as a success.
//
// The loop-level behaviour (nudge, then correct; budget exhaustion; tool
// contract taking precedence) needs a live model, and is covered end to end in
// tests/integration.sh against mock_llm.py.

#include "answer_schema.h"
#include <iostream>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

using nlohmann::json;

static const json kSchema = json::parse(R"({
    "type": "object",
    "required": ["summary", "sources"],
    "properties": {
        "summary": {"type": "string"},
        "sources": {"type": "array", "items": {"type": "string"}, "minItems": 1},
        "confidence": {"type": "string", "enum": ["low", "medium", "high"]}
    }
})");

int test_extraction() {
    json out;

    // Bare JSON.
    CHECK(funes::extract_answer_json(R"({"a": 1})", out));
    CHECK(out["a"] == 1);

    // Fenced, with and without a language tag — how local models actually reply.
    CHECK(funes::extract_answer_json("```json\n{\"a\": 2}\n```", out));
    CHECK(out["a"] == 2);
    CHECK(funes::extract_answer_json("```\n{\"a\": 3}\n```", out));
    CHECK(out["a"] == 3);

    // Prose around it. Not encouraged by the prompt, but not worth a nudge.
    CHECK(funes::extract_answer_json("Sure! Here you go: {\"a\": 4}. Anything else?", out));
    CHECK(out["a"] == 4);

    // Fence wins over the surrounding chatter.
    CHECK(funes::extract_answer_json("Here is the result:\n```json\n{\"a\": 5}\n```\nHope that helps!", out));
    CHECK(out["a"] == 5);

    // Arrays are values too.
    CHECK(funes::extract_answer_json("[1, 2, 3]", out));
    CHECK(out.is_array());

    // No JSON at all — the case that has to fail, so it can be nudged.
    CHECK(!funes::extract_answer_json("I looked into it and found three sources.", out));
    CHECK(!funes::extract_answer_json("", out));
    return 0;
}

int test_validation() {
    // Valid.
    CHECK(funes::validate_answer(kSchema, json::parse(
        R"({"summary": "it works", "sources": ["https://example.com"]})")).empty());
    CHECK(funes::validate_answer(kSchema, json::parse(
        R"({"summary": "s", "sources": ["a"], "confidence": "high"})")).empty());

    // Empty schema = no contract, anything goes (the regression bar: existing
    // agents must behave exactly as before).
    CHECK(funes::validate_answer(json::object(), json("just prose")).empty());
    CHECK(funes::validate_answer(json(), json("just prose")).empty());

    // Missing required field, named specifically enough to act on.
    std::string err = funes::validate_answer(kSchema, json::parse(R"({"summary": "s"})"));
    CHECK(!err.empty());
    CHECK(err.find("sources") != std::string::npos);

    // A required field present but null is missing in every way that matters.
    err = funes::validate_answer(kSchema, json::parse(R"({"summary": "s", "sources": null})"));
    CHECK(!err.empty());

    // Wrong top-level type.
    err = funes::validate_answer(kSchema, json("a string answer"));
    CHECK(!err.empty());
    CHECK(err.find("object") != std::string::npos);

    // Wrong property type, with the path.
    err = funes::validate_answer(kSchema, json::parse(R"({"summary": 42, "sources": ["a"]})"));
    CHECK(err.find("summary") != std::string::npos);

    // Wrong item type, with the index — "sources[1]", not just "sources".
    err = funes::validate_answer(kSchema, json::parse(
        R"({"summary": "s", "sources": ["ok", 7]})"));
    CHECK(err.find("sources[1]") != std::string::npos);

    // minItems: an empty list is the classic way a model answers "no sources"
    // while still matching the shape.
    err = funes::validate_answer(kSchema, json::parse(R"({"summary": "s", "sources": []})"));
    CHECK(!err.empty());

    // enum.
    err = funes::validate_answer(kSchema, json::parse(
        R"({"summary": "s", "sources": ["a"], "confidence": "certain"})"));
    CHECK(err.find("confidence") != std::string::npos);

    // Unknown keywords degrade to the supported subset instead of failing shut.
    json exotic = json::parse(R"({"type": "object", "patternProperties": {"^x": {}},
                                  "required": ["a"]})");
    CHECK(funes::validate_answer(exotic, json::parse(R"({"a": 1})")).empty());

    // integer vs number is a real distinction; string-vs-integer is the one
    // local models get wrong ("count": "3").
    json num = json::parse(R"({"type": "object", "properties": {"n": {"type": "integer"}}})");
    CHECK(funes::validate_answer(num, json::parse(R"({"n": 3})")).empty());
    CHECK(!funes::validate_answer(num, json::parse(R"({"n": "3"})")).empty());
    return 0;
}

int test_strings() {
    // The prompt block is generated from the schema so the two can't drift.
    std::string block = funes::schema_prompt_block(kSchema);
    CHECK(block.find("summary") != std::string::npos);
    CHECK(block.find("sources") != std::string::npos);
    CHECK(funes::schema_prompt_block(json::object()).empty());

    // The nudge names the specific violation — a generic "invalid format"
    // makes a small model rewrite the parts that were already right.
    std::string nudge = funes::schema_nudge("`sources`: required field is missing", kSchema);
    CHECK(nudge.find("sources") != std::string::npos);

    // The failure must not be mistakable for an answer by a delegating agent
    // that sees only this string.
    std::string fail = funes::schema_failure("`sources`: required field is missing",
                                             "I found three good articles.");
    CHECK(fail.find("FAILED") != std::string::npos);
    CHECK(fail.find("sources") != std::string::npos);
    CHECK(fail.find("I found three good articles.") != std::string::npos);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_extraction();
    rc |= test_validation();
    rc |= test_strings();
    if (rc == 0) std::cout << "test_answer_schema: all tests passed\n";
    return rc;
}
