// =============================================================================
// tests/test_classify_decision.cpp — classify_decision native tool
// =============================================================================
// Drives the tool through ToolRegistry::call, the same seam a real agent run
// uses, against a mock llama.cpp /completion server — never by reaching into
// the anonymous-namespace helpers in classify_decision.cpp directly.
//
// The two illustrative cases are the point of the feature, not incidental:
//   - a mock that always favors the first-listed option regardless of its
//     content (pure position bias) must wash out to ~uniform once both
//     option orderings are averaged;
//   - a mock whose answer tracks the option's *content* regardless of which
//     letter it landed on must survive averaging with its signal intact.
// If either regressed to reading only the first pass, both tests would still
// look plausible individually — that's why both are here together.

#include "agent.h"
#include "httplib.h"
#include "llm_client.h"
#include "tools.h"
#include <cmath>
#include <iostream>
#include <thread>

using json = nlohmann::json;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

void register_classify_decision_tool(ToolRegistry& reg, const AgentDefaults& defaults);

namespace {

bool approx(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

// Serves /completion with a response built from the request body by
// `responder`, so a test can make the mock's answer depend on which option
// ended up under which letter.
struct DynamicMockServer {
    httplib::Server srv;
    std::thread th;
    int port = 0;
    std::vector<std::string> bodies;

    void start(std::function<std::string(const std::string& body)> responder) {
        srv.Post("/completion", [this, responder](const httplib::Request& req, httplib::Response& res) {
            bodies.push_back(req.body);
            res.set_content(responder(req.body), "application/json");
        });
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        for (int i = 0; i < 50 && !srv.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ~DynamicMockServer() { srv.stop(); if (th.joinable()) th.join(); }
};

// letter is bare ("A", "B") — the space classify_decision's candidates
// actually carry (see the comment in classify_decision.cpp on why) is added
// here so call sites read naturally against the enumerated option lines.
std::string probs_favoring(const std::string& letter, double p) {
    json resp = {
        {"completion_probabilities", json::array({
            {{"top_logprobs", json::array({
                {{"token", " " + letter}, {"logprob", std::log(p)}}
            })}}
        })}
    };
    return resp.dump();
}

AgentDefaults defaults_for(int port) {
    AgentDefaults d;
    d.llm_url      = "http://127.0.0.1:" + std::to_string(port);
    d.llm_provider = "openai";
    return d;
}

ToolContext ctx() { return ToolContext("test-agent", "test-session"); }

ToolContext ctx_with_task(const std::string& task_text) {
    ToolContext c = ctx();
    c.task_text = task_text;
    return c;
}

} // namespace

int test_registers_with_expected_schema() {
    ToolRegistry reg;
    AgentDefaults defaults;  // never called — this test only inspects the schema
    register_classify_decision_tool(reg, defaults);

    CHECK(reg.has("classify_decision"));
    json schema = reg.openai_schema();
    bool found = false;
    for (const auto& t : schema) {
        if (t["function"]["name"] != "classify_decision") continue;
        found = true;
        const json& props = t["function"]["parameters"]["properties"];
        CHECK(props.contains("state"));
        CHECK(props.contains("question"));
        CHECK(props.contains("options"));
        // Deliberately optional, not required — see the task_text fallback
        // tests below. No "required" key at all means every property is
        // optional as far as the schema is concerned.
        CHECK(!t["function"]["parameters"].contains("required"));
    }
    CHECK(found);
    return 0;
}

int test_rejects_missing_and_malformed_arguments() {
    ToolRegistry reg;
    AgentDefaults defaults;  // unreachable — every case here fails validation first
    register_classify_decision_tool(reg, defaults);

    auto call = [&](json args) { return reg.call("classify_decision", args, ctx()); };

    CHECK(call({{"question", "q"}, {"options", {"a", "b"}}}).error);
    CHECK(call({{"state", "s"}, {"options", {"a", "b"}}}).error);
    CHECK(call({{"state", "s"}, {"question", "q"}, {"options", {"a"}}}).error);          // too few
    CHECK(call({{"state", "s"}, {"question", "q"},
                {"options", {"a", "b", "c", "d", "e", "f", "g"}}}).error);               // too many
    CHECK(call({{"state", "s"}, {"question", "q"}, {"options", {"a", ""}}}).error);      // empty option
    return 0;
}

int test_pure_position_bias_washes_out_to_uniform() {
    // The mock always puts all its mass on whichever option is currently
    // labeled "A" — content-blind. A tool that only read the first pass
    // would report ~100% for options[0]; averaging both orderings should
    // report ~50/50 instead, since "A" points at a different real option in
    // each pass.
    DynamicMockServer mock;
    mock.start([](const std::string&) { return probs_favoring("A", 0.95); });

    ToolRegistry reg;
    AgentDefaults defaults = defaults_for(mock.port);
    register_classify_decision_tool(reg, defaults);

    auto result = reg.call("classify_decision",
        {{"state", "a customer says the product broke"},
         {"question", "which team should handle this?"},
         {"options", {"billing", "technical"}}}, ctx());

    CHECK(!result.error);
    json out = json::parse(result.text);
    double p_billing = 0, p_technical = 0;
    for (const auto& o : out["options"]) {
        if (o["option"] == "billing") p_billing = o["probability"];
        if (o["option"] == "technical") p_technical = o["probability"];
    }
    CHECK(approx(p_billing, 0.5, 0.05));
    CHECK(approx(p_technical, 0.5, 0.05));
    CHECK(mock.bodies.size() == 2);  // both orderings were actually read
    return 0;
}

int test_real_signal_survives_averaging() {
    // The mock's answer tracks option *content*: whichever request body
    // contains "technical" as option A answers "A" strongly, and the pass
    // where "technical" is option B answers "B" strongly — i.e. it always
    // picks "technical" regardless of which letter it landed on. The tool
    // should report high confidence on "technical" after averaging, not a
    // washed-out 50/50.
    DynamicMockServer mock;
    mock.start([](const std::string& body) {
        bool technical_is_a = body.find("A) technical") != std::string::npos;
        return probs_favoring(technical_is_a ? "A" : "B", 0.9);
    });

    ToolRegistry reg;
    AgentDefaults defaults = defaults_for(mock.port);
    register_classify_decision_tool(reg, defaults);

    auto result = reg.call("classify_decision",
        {{"state", "a customer says they were charged twice"},
         {"question", "which team should handle this?"},
         {"options", {"billing", "technical"}}}, ctx());

    CHECK(!result.error);
    json out = json::parse(result.text);
    CHECK(out["top"] == "technical");
    double p_technical = 0;
    for (const auto& o : out["options"])
        if (o["option"] == "technical") p_technical = o["probability"];
    CHECK(p_technical > 0.8);
    return 0;
}

int test_llm_failure_becomes_error_result_not_a_crash() {
    DynamicMockServer mock;
    mock.start([](const std::string&) { return "not json"; });  // triggers a parse error in label_probs

    ToolRegistry reg;
    AgentDefaults defaults = defaults_for(mock.port);
    register_classify_decision_tool(reg, defaults);

    auto result = reg.call("classify_decision",
        {{"state", "s"}, {"question", "q"}, {"options", {"a", "b"}}}, ctx());
    CHECK(result.error);
    return 0;
}

int test_falls_back_to_task_text_when_arguments_are_omitted() {
    // No state/question/options in args at all — this is the whole point of
    // the fallback: classifier.yaml now tells the model to call with {},
    // relying entirely on ctx.task_text for the real content.
    DynamicMockServer mock;
    mock.start([](const std::string& body) {
        // Confirms the *parsed* state actually reached the LLM request, not
        // some empty/placeholder string — the prompt sent to /completion
        // must contain the real state text extracted from task_text.
        bool has_real_state = body.find("a customer says the product broke") != std::string::npos;
        return probs_favoring(has_real_state ? "A" : "B", 0.9);
    });

    ToolRegistry reg;
    AgentDefaults defaults = defaults_for(mock.port);
    register_classify_decision_tool(reg, defaults);

    auto result = reg.call("classify_decision", json::object(),
        ctx_with_task("State: a customer says the product broke\n"
                     "Question: which team should handle this?\n"
                     "Options: billing | technical"));

    CHECK(!result.error);
    json out = json::parse(result.text);
    CHECK(out["top"] == "billing");
    return 0;
}

int test_explicit_arguments_override_task_text() {
    // task_text describes a completely different decision than the explicit
    // arguments — if the fallback ever won over an explicit argument, this
    // mock (content-blind, always favors "A") would answer with whichever
    // option is first in task_text's Options: line instead of args'.
    DynamicMockServer mock;
    mock.start([](const std::string&) { return probs_favoring("A", 0.9); });

    ToolRegistry reg;
    AgentDefaults defaults = defaults_for(mock.port);
    register_classify_decision_tool(reg, defaults);

    auto result = reg.call("classify_decision",
        {{"state", "explicit state"}, {"question", "explicit question"},
         {"options", {"explicit-a", "explicit-b"}}},
        ctx_with_task("State: task-text state\nQuestion: task-text question\n"
                     "Options: task-text-a | task-text-b"));

    CHECK(!result.error);
    json out = json::parse(result.text);
    CHECK(out["top"] == "explicit-a");
    for (const auto& o : out["options"]) {
        std::string opt = o["option"];
        CHECK(opt == "explicit-a" || opt == "explicit-b");  // never the task_text options
    }
    return 0;
}

int test_unparseable_task_text_with_no_arguments_is_a_clean_error() {
    ToolRegistry reg;
    AgentDefaults defaults;  // unreachable — fails validation before any network call
    register_classify_decision_tool(reg, defaults);

    // No args, and task_text doesn't follow the State:/Question:/Options:
    // shape at all (e.g. a delegator that didn't use the documented
    // convention) — must fail cleanly, not crash or silently invent values.
    auto result = reg.call("classify_decision", json::object(),
        ctx_with_task("just some free-form text with no structure"));
    CHECK(result.error);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_registers_with_expected_schema();
    rc |= test_rejects_missing_and_malformed_arguments();
    rc |= test_pure_position_bias_washes_out_to_uniform();
    rc |= test_real_signal_survives_averaging();
    rc |= test_llm_failure_becomes_error_result_not_a_crash();
    rc |= test_falls_back_to_task_text_when_arguments_are_omitted();
    rc |= test_explicit_arguments_override_task_text();
    rc |= test_unparseable_task_text_with_no_arguments_is_a_clean_error();
    if (rc == 0) std::cout << "test_classify_decision: all tests passed\n";
    return rc;
}
