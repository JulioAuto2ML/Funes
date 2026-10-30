// =============================================================================
// tests/test_context_budget.cpp — keeping one run's transcript inside the window
// =============================================================================
// Context compression folds *earlier conversations* into a summary before a
// run starts; nothing bounded what a run accumulates while it runs. On
// 2026-10-30 curator's ES issue died at step ~26 with "request (32849 tokens)
// exceeds the available context size (32768 tokens)": three harvest pools and
// eight full publish_issue argument sets, all still in the transcript.
// trim_to_fit shrinks the oldest of that, deterministically, before each call.

#include "context_budget.h"
#include <iostream>

using json = nlohmann::json;
using funes::trim_to_fit;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static ChatMessage msg(const std::string& role, const std::string& content) {
    ChatMessage m;
    m.role = role;
    m.content = content;
    return m;
}

static ChatMessage tool_result(const std::string& id, const std::string& content) {
    ChatMessage m = msg("tool", content);
    m.tool_call_id = id;
    m.name = "harvest_candidates";
    return m;
}

static ChatMessage assistant_call(const std::string& id, const std::string& tool,
                                  const json& args) {
    ChatMessage m = msg("assistant", "");
    m.tool_calls = json::array({{
        {"id", id}, {"type", "function"},
        {"function", {{"name", tool}, {"arguments", args.dump()}}}
    }});
    return m;
}

static std::string big(char c, size_t n) { return std::string(n, c); }

// system, task, then two complete tool round trips, the second one current.
static std::vector<ChatMessage> two_round_trips() {
    return {
        msg("system", "You put together one issue."),
        msg("user", "Publish today's ai-pulse-es issue"),
        assistant_call("c1", "harvest_candidates", {{"publication", "ai-pulse-es"}}),
        tool_result("c1", big('a', 40000)),
        assistant_call("c2", "harvest_candidates", {{"time_range", "week"}}),
        tool_result("c2", big('b', 40000)),
    };
}

int test_under_budget_is_untouched() {
    auto h = two_round_trips();
    const auto before = h;
    const auto out = trim_to_fit(h, 1000000);
    CHECK(out.fits);
    CHECK(!out.trimmed());
    for (size_t i = 0; i < h.size(); ++i) CHECK(h[i].content == before[i].content);
    return 0;
}

// The oldest tool result goes first, and only as much as needed: here eliding
// it is enough, so the current step's result — the one the model is about to
// read — is left whole.
int test_oldest_tool_result_goes_first() {
    auto h = two_round_trips();
    const auto out = trim_to_fit(h, 12000);
    CHECK(out.fits);
    CHECK(out.tool_results == 1);
    CHECK(h[3].content.size() < 2000);
    CHECK(h[3].content.find("elided") != std::string::npos);
    CHECK(h[3].tool_call_id == "c1");          // the pairing the API checks
    CHECK(h[5].content == big('b', 40000));
    CHECK(out.after <= 12000);
    CHECK(out.before > out.after);
    return 0;
}

// The system prompt, the person's message and everything from the latest
// assistant turn on are never touched; when that alone is over budget the
// outcome says so rather than pretending.
int test_protected_messages_survive_and_fits_is_honest() {
    std::vector<ChatMessage> h = {
        msg("system", big('s', 20000)),
        msg("user", big('u', 20000)),
        assistant_call("c1", "read_result", {{"id", 1}}),
        tool_result("c1", big('r', 20000)),
    };
    const auto out = trim_to_fit(h, 1000);
    CHECK(!out.fits);
    CHECK(h[0].content.size() == 20000);
    CHECK(h[1].content.size() == 20000);
    CHECK(h[3].content.size() == 20000);
    return 0;
}

// After the tool results, an old call's own arguments: eight publish_issue
// calls with nine posts each were half of the 2026-10-30 transcript. The call
// stays a well-formed call — same id, same name, arguments still JSON — or
// the API rejects the history outright.
int test_old_call_arguments_are_elided_but_stay_valid() {
    json items = json::array();
    for (int i = 0; i < 9; ++i)
        items.push_back({{"id", i}, {"emoji", "📈"}, {"text", big('t', 2000)}});
    std::vector<ChatMessage> h = {
        msg("system", "sys"),
        msg("user", "task"),
        assistant_call("p1", "publish_issue", {{"items", items}}),
        tool_result("p1", "Not published — item 6 (id 4): duplicate"),
        assistant_call("p2", "publish_issue", {{"items", items}}),
        tool_result("p2", "Not published — item 6 (id 4): duplicate"),
    };
    const auto out = trim_to_fit(h, 6000);
    CHECK(out.fits);
    CHECK(out.tool_calls >= 1);
    const json& call = h[2].tool_calls[0];
    CHECK(call["id"] == "p1");
    CHECK(call["function"]["name"] == "publish_issue");
    const json args = json::parse(call["function"]["arguments"].get<std::string>());
    CHECK(args.is_object());
    // the current call is whole
    const json current = json::parse(h[4].tool_calls[0]["function"]["arguments"].get<std::string>());
    CHECK(current["items"].size() == 9);
    return 0;
}

// Images on an old tool message count ~800 tokens each and carry nothing a
// later step can use once the model has already looked at them.
int test_old_images_are_dropped() {
    std::vector<ChatMessage> h = {
        msg("system", "sys"),
        msg("user", "task"),
        assistant_call("c1", "read_pdf", {{"path", "a.pdf"}}),
        tool_result("c1", "page 1"),
        assistant_call("c2", "read_pdf", {{"path", "b.pdf"}}),
        tool_result("c2", "page 1"),
    };
    h[3].images.resize(10);
    const auto out = trim_to_fit(h, 2000);
    CHECK(out.fits);
    CHECK(h[3].images.empty());
    CHECK(out.images == 10);
    return 0;
}

// `overhead` is what the request carries besides the messages (the tools
// schema). It counts against the budget like anything else.
int test_overhead_counts_against_the_budget() {
    auto h = two_round_trips();
    const auto out = trim_to_fit(h, 26000, /*overhead_tokens=*/15000);
    CHECK(out.fits);
    CHECK(out.tool_results == 1);
    CHECK(out.after <= 26000);     // after includes the overhead
    return 0;
}

// Oldest-first is not quite right on its own: curator's newest harvest pool is
// older than the read_result pages it fetched afterwards, and it is the pool
// the issue gets published from. The newest result of each tool is kept until
// everything else has gone.
int test_newest_result_of_each_tool_goes_last() {
    auto read = [](const std::string& id, char c) {
        ChatMessage m = tool_result(id, big(c, 20000));
        m.name = "read_result";
        return m;
    };
    std::vector<ChatMessage> h = {
        msg("system", "sys"),
        msg("user", "task"),
        assistant_call("c1", "harvest_candidates", {{"publication", "x"}}),
        tool_result("c1", big('1', 20000)),                // older pool
        assistant_call("c2", "harvest_candidates", {{"time_range", "week"}}),
        tool_result("c2", big('2', 20000)),                // the pool in use
        assistant_call("c3", "read_result", {{"id", 1}}),
        read("c3", 'r'),
        assistant_call("c4", "read_result", {{"id", 2}}),  // step in progress
        read("c4", 's'),
    };
    const auto out = trim_to_fit(h, 12000);
    CHECK(out.fits);
    CHECK(out.tool_results == 2);
    CHECK(h[3].content.size() < 2000);       // older pool
    CHECK(h[7].content.size() < 2000);       // older read
    CHECK(h[5].content.size() == 20000);     // newest pool survives
    return 0;
}

int test_recognises_context_overflow_errors() {
    using funes::is_context_overflow;
    // llama.cpp, verbatim from yoda
    CHECK(is_context_overflow(
        "LLM API error 400: {\"error\":{\"code\":400,\"message\":\"request (32849 tokens) "
        "exceeds the available context size (32768 tokens), try increasing it\","
        "\"type\":\"exceed_context_size_error\"}}"));
    CHECK(is_context_overflow("LLM API error 400: This model's maximum context length is "
                              "128000 tokens. However, your messages resulted in 130000 tokens"));
    CHECK(is_context_overflow("Anthropic API error 400: prompt is too long: 210000 tokens > 200000 maximum"));
    CHECK(!is_context_overflow("LLM API error 500: internal server error"));
    CHECK(!is_context_overflow("HTTP request failed: Connection refused"));
    return 0;
}

int test_reported_prompt_tokens_are_parsed() {
    CHECK(funes::reported_prompt_tokens(
        "{\"n_prompt_tokens\":32849,\"n_ctx\":32768}") == 32849);
    CHECK(funes::reported_prompt_tokens("no numbers here") == 0);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_under_budget_is_untouched();
    rc |= test_oldest_tool_result_goes_first();
    rc |= test_protected_messages_survive_and_fits_is_honest();
    rc |= test_old_call_arguments_are_elided_but_stay_valid();
    rc |= test_old_images_are_dropped();
    rc |= test_overhead_counts_against_the_budget();
    rc |= test_newest_result_of_each_tool_goes_last();
    rc |= test_recognises_context_overflow_errors();
    rc |= test_reported_prompt_tokens_are_parsed();
    if (rc == 0) std::cout << "context_budget: all tests passed\n";
    return rc;
}
