// =============================================================================
// tests/test_completion_contract.cpp — completion-contract bookkeeping
// =============================================================================
// The loop wiring in agent.cpp needs a live LLM to exercise; these cover the
// decision logic it delegates to, which is where the off-by-one risks are.

#include "completion_contract.h"
#include <iostream>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int main() {
    using funes::CompletionContract;

    // No require_tools → contract inactive, nothing is ever withheld.
    CompletionContract none;
    CHECK(!none.active());
    CHECK(none.missing({}).empty());

    CompletionContract c{{"write_file", "execute_shell"}};
    CHECK(c.active());

    // Nothing called yet → everything missing, in declaration order.
    std::vector<std::string> m = c.missing({});
    CHECK(m.size() == 2);
    CHECK(m[0] == "write_file");
    CHECK(m[1] == "execute_shell");

    // Partially satisfied.
    m = c.missing({"write_file"});
    CHECK(m.size() == 1 && m[0] == "execute_shell");

    // Fully satisfied → the model is free to answer.
    CHECK(c.missing({"write_file", "execute_shell"}).empty());

    // Unrelated successful calls don't satisfy anything.
    m = c.missing({"read_file", "web_search"});
    CHECK(m.size() == 2);

    // A tool named twice in the prompt is one obligation, reported once.
    CompletionContract dup{{"write_file", "write_file", "execute_shell"}};
    m = dup.missing({});
    CHECK(m.size() == 2);
    CHECK(m[0] == "write_file" && m[1] == "execute_shell");
    // ...but it still buys nudge budget, since it's a longer task.
    CHECK(dup.nudge_budget() == 5);
    CHECK(c.nudge_budget() == 4);

    // The nudge has to name every outstanding tool — a model that only sees
    // the first one tends to call it and stop again.
    std::string nudge = funes::contract_nudge({"write_file", "execute_shell"});
    CHECK(nudge.find("write_file") != std::string::npos);
    CHECK(nudge.find("execute_shell") != std::string::npos);
    CHECK(!nudge.empty());

    // The failure string must name what was skipped and must not be mistakable
    // for success by a delegating agent that only reads this text.
    std::string fail = funes::contract_failure({"execute_shell"}, "All done, sent!");
    CHECK(fail.find("execute_shell") != std::string::npos);
    CHECK(fail.find("FAILED") != std::string::npos);
    // The model's own claim is preserved but clearly quoted as unverified.
    CHECK(fail.find("All done, sent!") != std::string::npos);

    std::cout << "test_completion_contract: all tests passed\n";
    return 0;
}
