// =============================================================================
// tests/test_auto_memory.cpp — what a memory is allowed to be (memory plan, phase 2)
// =============================================================================
// yoda, 2026-11-10: the largest auto memory was 14,492 chars — a whole résumé,
// attached in the UI and stored as the "User said" half of the exchange. Too
// long to embed (the 7.0.1 incident), a quarter of a small model's context if
// recalled, and its vector was the average of a document rather than the
// meaning of a question. The file is already in the workspace; the memory
// needs its name, not its contents.

#include "auto_memory.h"
#include "memory.h"
#include "tools.h"
#include <filesystem>
#include <iostream>
#include <string>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

void register_memory_tools(ToolRegistry& reg, MemoryStore& store);

namespace {

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// Every byte sequence in s is complete UTF-8.
bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > s.size()) return false;
        for (size_t j = 1; j < len; ++j)
            if ((static_cast<unsigned char>(s[i + j]) >> 6) != 2) return false;
        i += len;
    }
    return true;
}

} // namespace

// The UI inlines a text attachment as a fenced block under a marker line
// (ui/app.js, buildFullText). The memory keeps the marker's name, not the block.
int test_attachment_is_stored_as_its_name() {
    std::string body;
    for (int i = 0; i < 400; ++i) body += "Line " + std::to_string(i) + " of the résumé.\n";
    const std::string msg = "[Attached file: CV_2026.pdf]\n```\n" + body + "```\n\ncan you make this shorter?";
    CHECK(msg.size() > 8000);

    const std::string m = funes::auto_memory_text(msg, "Here is a shorter version.");
    CHECK(m.size() < 700);
    CHECK(contains(m, "[attached: CV_2026.pdf]"));
    CHECK(contains(m, "can you make this shorter?"));
    CHECK(!contains(m, "Line 12 of"));
    CHECK(!contains(m, "```"));
    return 0;
}

int test_image_and_binary_markers() {
    const std::string img = "[Attached image: IMG-0012.jpg]\n\nwhat is this?";
    CHECK(funes::auto_memory_text(img, "A mate gourd.")
          == "User said: \"[attached: IMG-0012.jpg] what is this?\" — I replied: \"A mate gourd.\"");

    const std::string bin = "[Attached file: Edenor.pdf]\n(binary file, saved to the workspace, not shown here)\n\nhow much?";
    const std::string m = funes::auto_memory_text(bin, "$48,320.");
    CHECK(contains(m, "[attached: Edenor.pdf] how much?"));
    CHECK(!contains(m, "binary file"));

    // An unclosed fence (a paste cut short) drops to the end rather than
    // keeping the document.
    const std::string open = "[Attached file: notes.txt]\n```\n" + std::string(5000, 'n');
    CHECK(funes::auto_memory_text(open, "ok").size() < 200);
    return 0;
}

// The user half is capped the way the reply half already was, at a sentence
// boundary when there is one.
int test_user_half_is_capped_on_a_sentence() {
    std::string msg;
    for (int i = 0; i < 30; ++i) msg += "This is sentence number " + std::to_string(i) + ". ";
    const std::string m = funes::auto_memory_text(msg, "ok");
    const size_t open = m.find('"') + 1;
    const std::string user_half = m.substr(open, m.find("\" — I replied") - open);
    CHECK(user_half.size() <= 304);           // 300 + the ellipsis
    CHECK(contains(user_half, "sentence number 0."));
    const std::string tail = ". …";            // cut after a full stop, not mid-sentence
    CHECK(user_half.compare(user_half.size() - tail.size(), tail.size(), tail) == 0);
    return 0;
}

// The old substr(0, 300) cut through multi-byte characters; a stored memory
// with half a "ñ" in it fails json::dump downstream.
int test_cut_never_splits_a_character() {
    std::string reply;
    while (reply.size() < 900) reply += "ñandú ";
    const std::string m = funes::auto_memory_text("hola", reply);
    CHECK(valid_utf8(m));
    CHECK(m.size() < 400);

    std::string word;                          // no spaces at all
    while (word.size() < 900) word += "ñ";
    CHECK(valid_utf8(funes::shorten_on_sentence(word, 301)));
    CHECK(funes::shorten_on_sentence(word, 301).size() <= 301 + 3);
    return 0;
}

int test_short_exchange_unchanged() {
    CHECK(funes::auto_memory_text("me llamo Julio", "¡Encantado, Julio!")
          == "User said: \"me llamo Julio\" — I replied: \"¡Encantado, Julio!\"");
    return 0;
}

// remember is for one sentence; a paragraph is a file.
int test_remember_tool_refuses_a_paragraph() {
    namespace fs = std::filesystem;
    const fs::path db = fs::temp_directory_path() / "funes_test_auto_memory.db";
    fs::remove(db);
    {
        MemoryStore store(db.string(), nullptr);
        ToolRegistry reg;
        register_memory_tools(reg, store);
        ToolContext ctx{"funes", "s1"};

        const auto long_one = reg.call("remember", {{"text", std::string(600, 'x')}}, ctx);
        CHECK(long_one.error);
        CHECK(contains(long_one.text, "500"));
        CHECK(store.count(1, "funes") == 0);

        // 500 characters, not bytes: Spanish text is not penalised for its accents.
        std::string accented;
        while (accented.size() < 900) accented += "ñ";   // 450 characters, 900 bytes
        CHECK(!reg.call("remember", {{"text", accented}}, ctx).error);
        CHECK(!reg.call("remember", {{"text", "Julio prefers mate to coffee."}}, ctx).error);
        CHECK(store.count(1, "funes") == 2);
    }
    fs::remove(db);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_attachment_is_stored_as_its_name();
    rc |= test_image_and_binary_markers();
    rc |= test_user_half_is_capped_on_a_sentence();
    rc |= test_cut_never_splits_a_character();
    rc |= test_short_exchange_unchanged();
    rc |= test_remember_tool_refuses_a_paragraph();
    if (rc == 0) std::cout << "test_auto_memory: all tests passed\n";
    return rc;
}
