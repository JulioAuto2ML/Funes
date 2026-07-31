// =============================================================================
// tests/test_run_outcome.cpp — how a run reports that it has no answer
// =============================================================================
// The point of these messages is that a *caller* can tell them apart from an
// answer. A sub-agent that gives up returns a string like any other, so the
// marker is the only thing standing between "this run failed" and the parent
// relaying the failure text to the user as content. See delegation.cpp.

#include "run_outcome.h"
#include "completion_contract.h"
#include "answer_schema.h"
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
    // Every builder is recognisable as a failure.
    CHECK(funes::is_run_failure(funes::empty_answer_failure("web_search")));
    CHECK(funes::is_run_failure(funes::loop_failure("read_file", 3)));
    CHECK(funes::is_run_failure(funes::max_steps_failure(20, "web_fetch")));

    // …including the two that predate this header. They already used the same
    // prefix by convention; is_run_failure is what makes that load-bearing, so
    // if someone reworks their wording these fail rather than silently
    // dropping out of the delegation error path.
    CHECK(funes::is_run_failure(funes::contract_failure({"write_file"}, "")));
    CHECK(funes::is_run_failure(funes::schema_failure("expected object", "")));

    // A real answer is not a failure — including one that merely talks about
    // failing, which a model doing an honest post-mortem will do.
    CHECK(!funes::is_run_failure("I could not find the paper you meant."));
    CHECK(!funes::is_run_failure("The build FAILED — here is the compiler output."));
    CHECK(!funes::is_run_failure(""));

    // The messages say what went wrong and name the tool, so the journal and
    // the parent agent both get something actionable.
    const std::string empty = funes::empty_answer_failure("web_search");
    CHECK(mentions(empty, "web_search"));

    const std::string loop = funes::loop_failure("read_file", 3);
    CHECK(mentions(loop, "read_file"));
    CHECK(mentions(loop, "3"));

    const std::string steps = funes::max_steps_failure(20, "web_fetch");
    CHECK(mentions(steps, "20"));
    CHECK(mentions(steps, "web_fetch"));

    // The whole point of the change: a bailout must not carry tool output.
    // These take a tool *name*, never its text, so there is no parameter a
    // scraped page could arrive through. Guard the wording that promised
    // otherwise — "Done." at the front of a giving-up message is what let a
    // raw web dump pass for a finished newsletter.
    for (const std::string& m : {empty, loop, steps}) {
        CHECK(!mentions(m, "Done."));
        CHECK(!mentions(m, "completed:"));
        CHECK(!mentions(m, "Last result:"));
    }

    // A bailout with no tool called yet still reads sensibly.
    CHECK(funes::is_run_failure(funes::empty_answer_failure("")));
    CHECK(funes::is_run_failure(funes::max_steps_failure(5, "")));

    std::cout << "test_run_outcome: all checks passed\n";
    return 0;
}
