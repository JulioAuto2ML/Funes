// =============================================================================
// tests/test_chat_runs.cpp — stopping a chat run is a request, not a hang-up
// =============================================================================
// The registry behind POST /api/chat/stop and GET /api/chat/active
// (core/chat_runs.h). What matters: a stop reaches every run in that account's
// conversation and nothing else — not another conversation, and not another
// account that happens to use the same session id.

#include "chat_runs.h"
#include <iostream>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int main() {
    funes::ChatRuns runs;
    CHECK(!runs.running(1, "s"));
    CHECK(!runs.stop(1, "s"));   // nothing to stop

    auto a = runs.start(1, "s");
    auto b = runs.start(1, "s");   // a second message while the first works
    auto other_session = runs.start(1, "t");
    auto other_user = runs.start(2, "s");
    CHECK(runs.running(1, "s"));
    CHECK(!a->load());

    CHECK(runs.stop(1, "s"));
    CHECK(a->load() && b->load());
    CHECK(!other_session->load());
    CHECK(!other_user->load());

    runs.finish(1, "s", a);
    CHECK(runs.running(1, "s"));   // b is still there
    runs.finish(1, "s", b);
    CHECK(!runs.running(1, "s"));
    CHECK(!runs.stop(1, "s"));
    CHECK(runs.running(2, "s"));

    runs.finish(1, "s", b);        // finishing twice is harmless
    runs.finish(2, "s", other_user);
    runs.finish(1, "t", other_session);
    CHECK(!runs.running(2, "s") && !runs.running(1, "t"));

    std::cout << "chat_runs: all tests passed\n";
    return 0;
}
