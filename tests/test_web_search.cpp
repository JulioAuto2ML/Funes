// =============================================================================
// tests/test_web_search.cpp — web_search's output shape
// =============================================================================
// A search result is a list of leads, not a document: the model's next act is
// to pick one and web_fetch it. On 2026-10-26 eight full Tavily extracts came
// to 11 KB, went to the result store, and researcher then funes paged the
// snippets back 2 KB at a time until the loop detector killed both runs. So
// snippets are capped and the tool is exempt from the store — these pin both.

#include "tools.h"
#include "tools/tavily.h"
#include <iostream>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

void register_web_search(ToolRegistry& reg);

static funes::tavily::Hit hit(int i, const std::string& snippet) {
    return {"Title " + std::to_string(i), snippet,
            "https://example.com/" + std::to_string(i), ""};
}

int main() {
    using funes::tavily::format_hits;
    using funes::tavily::kSnippetLimit;

    // A short snippet passes through untouched, title and URL around it.
    const std::string one = format_hits({hit(1, "short")}, 8);
    CHECK(one == "Title 1\nshort\nhttps://example.com/1");

    // A known publication date follows the title — recency is half of what a
    // research question about "2026" needs, and Parallel dates most hits.
    funes::tavily::Hit dated = hit(2, "s");
    dated.published = "2026-09-07";
    CHECK(format_hits({dated}, 8) == "Title 2 (2026-09-07)\ns\nhttps://example.com/2");

    // max_results caps the count, whatever the transport returned.
    std::vector<funes::tavily::Hit> many;
    for (int i = 0; i < 12; ++i) many.push_back(hit(i, std::string(1500, 'x')));
    const std::string three = format_hits(many, 3);
    CHECK(three.find("Title 2") != std::string::npos);
    CHECK(three.find("Title 3") == std::string::npos);

    // A long snippet is cut, and the cut is marked so the model knows to fetch.
    const std::string cut = format_hits({hit(1, std::string(1500, 'x'))}, 8);
    CHECK(cut.find(std::string(kSnippetLimit + 1, 'x')) == std::string::npos);
    CHECK(cut.find("…") != std::string::npos);

    // Ten hits of full Tavily extracts stay bounded — the promise inline_result
    // makes. 11 KB for eight is what started this.
    const std::string ten = format_hits(many, 10);
    CHECK(ten.size() < 10 * (kSnippetLimit + 64));

    // A backend can ask for a longer cut (Parallel's excerpts are the content).
    const std::string longer = format_hits({hit(1, std::string(1500, 'x'))}, 8, 600);
    CHECK(longer.find(std::string(600, 'x')) != std::string::npos);
    CHECK(longer.find(std::string(601, 'x')) == std::string::npos);

    // Cut on a UTF-8 boundary: "é" is two bytes, so an odd byte cap would
    // otherwise split one, and the result has to survive json::dump().
    std::string accents = "a";
    for (int i = 0; i < 400; ++i) accents += "é";
    const std::string utf = format_hits({hit(1, accents)}, 8);
    CHECK(nlohmann::json(utf).dump().size() > 0);

    // Exempt from the result store: a preview of a list of leads shows two of
    // them and a byte count.
    ToolRegistry reg;
    register_web_search(reg);
    CHECK(reg.inline_result("web_search"));

    std::cout << "test_web_search: all checks passed\n";
    return 0;
}
