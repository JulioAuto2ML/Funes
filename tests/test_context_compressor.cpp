// =============================================================================
// tests/test_context_compressor.cpp — token estimator + summary/prune storage
// =============================================================================
// The LLM-calling half of compress_oldest_half needs a live backend, so this
// only covers what's testable offline: the token estimator, the no-op guard
// (nothing folded when the window is already small), and the MemoryStore
// storage primitives it depends on (prune_turns, get/set_summary).

#include "context_compressor.h"
#include "memory.h"
#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_estimate_tokens() {
    CHECK(estimate_tokens("") == 0);
    CHECK(estimate_tokens("abcd") == 1);       // 4 chars / 4
    CHECK(estimate_tokens(std::string(400, 'x')) == 100);

    std::vector<ChatMessage> msgs;
    ChatMessage a; a.role = "user"; a.content = std::string(40, 'x');
    ChatMessage b; b.role = "assistant"; b.content = std::string(60, 'y');
    msgs.push_back(a);
    msgs.push_back(b);
    CHECK(estimate_tokens(msgs) == 25);  // (40+60)/4
    return 0;
}

int test_no_op_below_min_keep() {
    fs::path db = fs::temp_directory_path() / "funes_test_compressor.db";
    fs::remove(db);
    MemoryStore store(db.string(), nullptr);

    // No embedder means llm.complete() would need a real network call —
    // exercise only the path that returns before ever calling it.
    std::vector<ChatMessage> recent;
    ChatMessage m; m.role = "user"; m.content = "hi";
    recent.push_back(m);
    std::string summary;

    LLMClient llm("http://localhost:1", "", "default", "openai");
    CompressOutcome out = compress_oldest_half(store, llm, "s1", "funes", recent, summary, 4);
    CHECK(!out.compressed);
    CHECK(recent.size() == 1);  // untouched
    CHECK(summary.empty());
    return 0;
}

int test_prune_and_summary_storage() {
    fs::path db = fs::temp_directory_path() / "funes_test_compressor2.db";
    fs::remove(db);
    MemoryStore store(db.string(), nullptr);

    for (int i = 0; i < 6; ++i) {
        store.append_turn("s1", "funes", "user", "msg" + std::to_string(i));
        store.append_turn("s1", "funes", "assistant", "reply" + std::to_string(i));
    }
    CHECK(store.turn_count("s1") == 12);

    store.prune_turns("s1", 4);
    CHECK(store.turn_count("s1") == 4);
    auto remaining = store.recent_turns("s1", 100);
    CHECK(remaining.size() == 4);
    // The most recent 4 turns survive, oldest first within that window.
    CHECK(remaining.front().content == "msg4");
    CHECK(remaining.back().content == "reply5");

    CHECK(store.get_summary("s1").empty());
    store.set_summary("s1", "funes", "first summary");
    CHECK(store.get_summary("s1") == "first summary");
    store.set_summary("s1", "funes", "replaced, not appended");
    CHECK(store.get_summary("s1") == "replaced, not appended");
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_estimate_tokens();
    rc |= test_no_op_below_min_keep();
    rc |= test_prune_and_summary_storage();
    if (rc == 0) std::cout << "test_context_compressor: all tests passed\n";
    return rc;
}
