// =============================================================================
// tests/test_tool_call_recovery.cpp — tool calls written into content
// =============================================================================
// Local models sometimes write a tool call as text instead of a native call;
// LLMClient::recover_tool_calls_from_content turns it back into one. These
// cover the XML-ish <function=..><parameter=..> fallback.

#include "llm_client.h"
#include <iostream>

using json = nlohmann::json;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static CompletionResponse with_content(const std::string& content) {
    CompletionResponse r;
    r.content = content;
    return r;
}

// An XML parameter is text, so an array argument arrives as its JSON spelling.
// On 2026-10-30 curator's publish_issue calls came back this way five times in
// a row and each was refused as "Missing 'items'" — the call was right, the
// recovery handed the tool a string.
int test_xml_array_parameter_is_decoded() {
    auto r = with_content(
        "<tool_call>\n<function=publish_issue>\n"
        "<parameter=publication>ai-pulse-es</parameter>\n"
        "<parameter=items>\n[{\"id\": 4, \"emoji\": \"📈\", \"text\": \"hola\"}]\n</parameter>\n"
        "</function>\n</tool_call>");
    LLMClient::recover_tool_calls_from_content(r, false);
    CHECK(r.tool_calls.size() == 1);
    const json& args = r.tool_calls[0].arguments;
    CHECK(args["publication"] == "ai-pulse-es");
    CHECK(args["items"].is_array());
    CHECK(args["items"][0]["id"] == 4);
    return 0;
}

int test_xml_object_parameter_is_decoded() {
    auto r = with_content(
        "<function=schedule_job><parameter=spec>{\"kind\": \"script\"}</parameter></function>");
    LLMClient::recover_tool_calls_from_content(r, false);
    CHECK(r.tool_calls.size() == 1);
    CHECK(r.tool_calls[0].arguments["spec"].is_object());
    CHECK(r.tool_calls[0].arguments["spec"]["kind"] == "script");
    return 0;
}

// Only arrays and objects: a bare number or word stays a string, because the
// recovery has no schema to say a "2026" query was meant as an integer.
int test_xml_scalar_and_broken_values_stay_strings() {
    auto r = with_content(
        "<function=web_search>"
        "<parameter=query>2026</parameter>"
        "<parameter=note>[not json</parameter>"
        "</function>");
    LLMClient::recover_tool_calls_from_content(r, false);
    CHECK(r.tool_calls.size() == 1);
    CHECK(r.tool_calls[0].arguments["query"] == "2026");
    CHECK(r.tool_calls[0].arguments["note"] == "[not json");
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_xml_array_parameter_is_decoded();
    rc |= test_xml_object_parameter_is_decoded();
    rc |= test_xml_scalar_and_broken_values_stay_strings();
    if (rc == 0) std::cout << "tool_call_recovery: all tests passed\n";
    return rc;
}
