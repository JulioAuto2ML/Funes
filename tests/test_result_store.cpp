// =============================================================================
// tests/test_result_store.cpp — large tool results kept out of the transcript
// =============================================================================
// Covers the three pieces of the feature that can be tested without an LLM:
// the storage (session isolation is a security property, not a nicety), the
// preview shape, and the read_result window. The threshold decision itself
// lives in run_loop; what's asserted here is that the predicate it uses has
// the boundary exactly where the store's contract says it does.

#include "memory.h"
#include "result_store.h"
#include "tools.h"
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

// The user every fixture below acts as. Multi-user isolation gets its
// own explicit two-user tests; everything else just needs an owner.
static constexpr int64_t U1 = 1;

static std::string temp_db(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("funes_test_") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

int test_store_and_isolation() {
    MemoryStore store(temp_db("results"), nullptr);

    const std::string big(5000, 'x');
    int64_t id = store.store_result(U1, "s1", "funes", "web_fetch", big);
    CHECK(id > 0);

    auto got = store.get_result(U1, "s1", id);
    CHECK(got.has_value());
    CHECK(*got == big);

    // The session predicate is the isolation boundary: a valid id from another
    // conversation must be indistinguishable from a nonexistent one.
    CHECK(!store.get_result(U1, "s2", id).has_value());
    CHECK(!store.get_result(U1, "s1", id + 999).has_value());

    // Independent per session.
    int64_t other = store.store_result(U1, "s2", "funes", "read_file", "small");
    CHECK(other != id);
    CHECK(store.get_result(U1, "s2", other).has_value());

    // Pruning a session takes its results and nobody else's.
    CHECK(store.prune_results(U1, "s1") == 1);
    CHECK(!store.get_result(U1, "s1", id).has_value());
    CHECK(store.get_result(U1, "s2", other).has_value());

    // Age-based sweep: nothing is old enough yet, everything is at 0 days.
    CHECK(store.prune_results_older_than(7) == 0);
    CHECK(store.prune_results_older_than(0) == 1);
    CHECK(!store.get_result(U1, "s2", other).has_value());
    return 0;
}

int test_threshold_boundary() {
    // Exactly at the limit stays inline; one byte over does not. The cost of
    // getting this backwards is either storing everything or storing nothing.
    CHECK(!funes::exceeds_inline_limit(std::string(funes::kInlineLimit - 1, 'a')));
    CHECK(!funes::exceeds_inline_limit(std::string(funes::kInlineLimit, 'a')));
    CHECK(funes::exceeds_inline_limit(std::string(funes::kInlineLimit + 1, 'a')));
    CHECK(!funes::exceeds_inline_limit(""));
    return 0;
}

int test_preview() {
    std::string text = "HEADSTART" + std::string(40000, 'm') + "TAILEND";
    std::string preview = funes::result_preview(17, "web_fetch", text);

    // The shape has to be explicit — a model reads the size and the id and
    // understands the handle refers to a real object.
    CHECK(preview.find("\"result_id\":17") != std::string::npos);
    CHECK(preview.find("\"tool\":\"web_fetch\"") != std::string::npos);
    CHECK(preview.find("\"bytes\":" + std::to_string(text.size())) != std::string::npos);
    CHECK(preview.find("HEADSTART") != std::string::npos);
    CHECK(preview.find("TAILEND") != std::string::npos);
    CHECK(preview.find("read_result") != std::string::npos);
    // Whole point: the preview is small, whatever the result was.
    CHECK(preview.size() < 3000);

    // Multibyte input must not be cut mid-character — the preview is dumped
    // into a JSON request body, and a split UTF-8 sequence would throw there.
    std::string multibyte;
    while (multibyte.size() < 40000) multibyte += "áé€漢字";
    std::string mb_preview = funes::result_preview(1, "read_file", multibyte);
    CHECK(mb_preview.find("�") == std::string::npos);   // no replacement chars
    CHECK(nlohmann::json::accept(mb_preview));
    return 0;
}

int test_window() {
    std::string text;
    for (int i = 0; i < 1000; ++i) text += "line" + std::to_string(i) + "\n";

    std::string first = funes::result_window(text, 0, 100);
    CHECK(first.rfind("line0\n", 0) == 0);
    CHECK(first.find("bytes remain") != std::string::npos);
    CHECK(first.find("continue with offset=100") != std::string::npos);

    // Reading to the end says so, instead of inviting another call.
    std::string all = funes::result_window(text, 0, text.size() + 10);
    CHECK(all.find("end of result") != std::string::npos);

    // Past the end is an explicit, non-empty answer: an empty string reads to
    // a model as "the content was empty".
    std::string past = funes::result_window(text, text.size() + 5, 100);
    CHECK(past.find("past the end") != std::string::npos);

    // Windows into multibyte text stay on character boundaries at both ends.
    std::string mb;
    while (mb.size() < 500) mb += "漢字テスト";
    std::string chunk = funes::result_window(mb, 4, 50);   // 4 lands mid-character
    CHECK(nlohmann::json(chunk).dump().size() > 0);        // would throw if invalid
    return 0;
}

int test_read_result_tool() {
    MemoryStore store(temp_db("read_result"), nullptr);
    ToolRegistry reg;
    register_result_tools(reg, store);
    CHECK(reg.has("read_result"));

    const std::string text(10000, 'z');
    const int64_t id = store.store_result(U1, "s1", "funes", "web_fetch", text);

    ToolContext ctx{"funes", "s1", ""};
    ToolResult r = reg.call("read_result", {{"id", id}}, ctx);
    CHECK(!r.error);
    // Never more than the inline limit, or one dereference re-imports what the
    // preview just evicted.
    CHECK(r.text.size() <= funes::kInlineLimit + 100);   // + the trailing note
    CHECK(r.text.find("continue with offset=") != std::string::npos);

    ToolResult windowed = reg.call("read_result", {{"id", id}, {"offset", 9990}, {"limit", 50}}, ctx);
    CHECK(!windowed.error);
    CHECK(windowed.text.find("end of result") != std::string::npos);

    // An oversized limit is clamped, not honoured.
    ToolResult greedy = reg.call("read_result", {{"id", id}, {"limit", 999999}}, ctx);
    CHECK(!greedy.error);
    CHECK(greedy.text.size() <= funes::kInlineLimit + 100);

    // Another session's id is a plain not-found, with an explanation the model
    // can act on rather than retry blindly.
    ToolContext other{"funes", "s2", ""};
    ToolResult denied = reg.call("read_result", {{"id", id}}, other);
    CHECK(denied.error);
    CHECK(denied.text.find("No stored result") != std::string::npos);

    CHECK(reg.call("read_result", json::object(), ctx).error);                  // no id
    CHECK(reg.call("read_result", {{"id", id}, {"offset", -1}}, ctx).error);
    CHECK(reg.call("read_result", {{"id", id}, {"limit", 0}}, ctx).error);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_store_and_isolation();
    rc |= test_threshold_boundary();
    rc |= test_preview();
    rc |= test_window();
    rc |= test_read_result_tool();
    if (rc == 0) std::cout << "test_result_store: all tests passed\n";
    return rc;
}
