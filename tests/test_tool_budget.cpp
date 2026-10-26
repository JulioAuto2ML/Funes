// =============================================================================
// tests/test_tool_budget.cpp — per-tool call ceilings
// =============================================================================
// The counterpart of the completion contract: require_tools says a call must
// happen, tool_limits says it may not happen more than N times. The behaviour
// that matters is that going over budget is *recoverable* — the model is told
// to conclude with what it has, rather than the run being killed — so these
// pin down the boundary and the wording that carries that instruction.

#include "tool_budget.h"
#include <iostream>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static bool mentions(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int main() {
    const funes::ToolLimits limits{{"web_search", 5}};

    // An unlimited tool is never over budget, however often it is called.
    CHECK(!funes::over_budget(limits, "web_fetch", 1));
    CHECK(!funes::over_budget(limits, "web_fetch", 999));

    // No limits declared at all — the common case, must stay free.
    CHECK(!funes::over_budget({}, "web_search", 999));

    // The boundary: the 5th call still runs, the 6th does not. `calls` counts
    // the call being considered, so off-by-one here is the whole feature.
    CHECK(!funes::over_budget(limits, "web_search", 4));
    CHECK(!funes::over_budget(limits, "web_search", 5));
    CHECK(funes::over_budget(limits, "web_search", 6));
    CHECK(funes::over_budget(limits, "web_search", 7));

    // A limit of 0 forbids the tool outright — a way to take a tool away from
    // one run without editing the agent's tool list.
    CHECK(funes::over_budget({{"web_search", 0}}, "web_search", 1));

    // A negative limit is a config typo, not "infinite" and not "forbidden".
    // Treat it as no limit rather than silently disabling a tool.
    CHECK(!funes::over_budget({{"web_search", -1}}, "web_search", 1));

    const std::string msg = funes::budget_message("web_search", 5);
    CHECK(mentions(msg, "web_search"));
    CHECK(mentions(msg, "5"));
    // It has to say the door is closed — a model that reads this as a
    // transient error will just retry and burn the rest of its steps.
    CHECK(mentions(msg, "will not run"));
    // …and say what to do instead, which is the entire point of choosing a
    // forcing function over killing the run.
    CHECK(mentions(msg, "Answer now"));

    // effective_limit: the ceiling a call is held to when the agent loop also
    // has a default of its own (max_same_tool). On 2026-10-26 read_result had
    // no entry, so the only thing that ever stopped it was the loop detector,
    // which killed researcher and then funes with no answer. A default
    // ceiling makes that a refusal the model can conclude from instead.
    CHECK(funes::effective_limit(limits, "web_search", 20) == 5);   // declared wins
    CHECK(funes::effective_limit(limits, "read_result", 20) == 20); // default applies
    CHECK(funes::effective_limit({{"web_search", 0}}, "web_search", 20) == 0);
    // A typo'd negative entry is "not declared", so the default still holds —
    // it must not turn a tool into one nothing can stop.
    CHECK(funes::effective_limit({{"web_search", -1}}, "web_search", 20) == 20);
    // A negative default means none: qualified script keys only have a
    // ceiling when one is written down.
    CHECK(funes::effective_limit(limits, "run_script:x", -1) < 0);
    // A declared limit above the default is honoured — the author said so.
    CHECK(funes::effective_limit({{"web_fetch", 30}}, "web_fetch", 20) == 30);

    std::cout << "test_tool_budget: all checks passed\n";
    return 0;
}
