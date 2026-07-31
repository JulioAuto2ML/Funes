// =============================================================================
// tests/test_harvest.cpp — the candidate pool, minus the network
// =============================================================================
// harvest_candidates has one job the newsletter's correctness now rests on:
// whatever reaches the pool must be fresh, distinct, and reachable, and the
// numbering must be dense so a model can't ask for an id that doesn't exist.
// Everything asserted here is a step in that, and every step is a pure
// function precisely so it can be asserted without a Tavily key.
//
// The searching and fetching are not tested here — they are two HTTP calls
// already covered by test_web_fetch, and a test that needs live news is not a
// test. What is tested is every decision made about a hit *between* those two
// calls, which is where the pool's guarantees actually come from.

#include "tools/harvest.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace funes::harvest;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static funes::tavily::Hit hit(const std::string& title, const std::string& url,
                              const std::string& published = "") {
    funes::tavily::Hit h;
    h.title = title;
    h.url = url;
    h.published = published;
    return h;
}

// ── URLs ─────────────────────────────────────────────────────────────────────

int test_strip_tracking() {
    CHECK(strip_tracking("https://a.com/x") == "https://a.com/x");
    CHECK(strip_tracking("https://a.com/x?utm_source=news&id=7") == "https://a.com/x?id=7");
    CHECK(strip_tracking("https://a.com/x?utm_source=n") == "https://a.com/x");
    CHECK(strip_tracking("https://a.com/x?fbclid=abc&gclid=d") == "https://a.com/x");

    // A parameter that might carry meaning to the site is left alone: dropping
    // a load-bearing one turns a good link into a 404.
    CHECK(strip_tracking("https://a.com/story?id=7&ref=home") ==
          "https://a.com/story?id=7&ref=home");

    // The fragment survives — some are real anchors into the article.
    CHECK(strip_tracking("https://a.com/x?utm_medium=rss#section-2") ==
          "https://a.com/x#section-2");
    return 0;
}

int test_canonical_url() {
    // Everything that makes two links to one article look different.
    const std::string want = "reuters.com/tech/story";
    CHECK(canonical_url("https://www.reuters.com/tech/story") == want);
    CHECK(canonical_url("http://reuters.com/tech/story/") == want);
    CHECK(canonical_url("https://REUTERS.com/tech/story#top") == want);
    CHECK(canonical_url("https://www.reuters.com/tech/story?utm_source=x") == want);

    // Different articles must not collide.
    CHECK(canonical_url("https://a.com/one") != canonical_url("https://a.com/two"));
    // A meaningful query parameter is part of the identity.
    CHECK(canonical_url("https://a.com/s?id=1") != canonical_url("https://a.com/s?id=2"));
    return 0;
}

int test_host_of() {
    CHECK(host_of("https://www.engadget.com/a/b") == "engadget.com");
    CHECK(host_of("http://blogs.nvidia.com:8080/x") == "blogs.nvidia.com");
    CHECK(host_of("https://DeepMind.Google/blog") == "deepmind.google");
    return 0;
}

int test_title_key() {
    // The same wire story under two house styles has to collide.
    CHECK(title_key("OpenAI Cuts Prices") == title_key("openai cuts prices!"));
    CHECK(title_key("OpenAI Cuts Prices") != title_key("OpenAI Raises Prices"));
    CHECK(title_key("  ") .empty());
    return 0;
}

// ── Query resolution ─────────────────────────────────────────────────────────
// The bug this covers shipped in the tool's own JSON schema, not in this
// function: `queries` was marked required, so a model could never actually
// follow the description's "omit to use the configured queries" — every live
// run on 2026-07-31 either sent `[]` (rejected here, before this fix) or
// invented generic queries instead of the publication's own. Fixed on both
// sides: the schema no longer requires it, and an empty array here is treated
// the same as an absent key.

int test_resolve_queries_omitted_key_uses_config() {
    std::vector<std::string> out;
    const std::string configured_are = resolve_queries(
        nlohmann::json::object(), {"a", "b"}, "ai-pulse", out);
    CHECK(configured_are.empty());
    CHECK(out == std::vector<std::string>({"a", "b"}));
    return 0;
}

int test_resolve_queries_empty_array_falls_back_to_config() {
    // The case that broke every live run: a model reaching for `[]` to mean
    // "I have none of my own" must get the configured queries, not an error.
    std::vector<std::string> out;
    const std::string problem = resolve_queries(
        nlohmann::json{{"queries", nlohmann::json::array()}}, {"a", "b"}, "ai-pulse", out);
    CHECK(problem.empty());
    CHECK(out == std::vector<std::string>({"a", "b"}));
    return 0;
}

int test_resolve_queries_own_queries_win() {
    std::vector<std::string> out;
    const std::string problem = resolve_queries(
        nlohmann::json{{"queries", nlohmann::json::array({"x", "y", "z"})}},
        {"a", "b"}, "ai-pulse", out);
    CHECK(problem.empty());
    CHECK(out == std::vector<std::string>({"x", "y", "z"}));
    return 0;
}

int test_resolve_queries_no_queries_anywhere_is_an_error() {
    // No config and nothing supplied has to fail loudly — there is nothing to
    // search with, and silently returning zero queries would harvest nothing
    // while looking like success.
    std::vector<std::string> out;
    const std::string problem = resolve_queries(nlohmann::json::object(), {}, "bare", out);
    CHECK(!problem.empty());
    CHECK(problem.find("bare") != std::string::npos);
    return 0;
}

int test_resolve_queries_caps_at_six() {
    std::vector<std::string> out;
    resolve_queries(nlohmann::json::object(),
                    {"1", "2", "3", "4", "5", "6", "7", "8"}, "ai-pulse", out);
    CHECK(out.size() == 6);
    return 0;
}

int test_resolve_queries_rejects_the_wrong_type() {
    std::vector<std::string> out;
    CHECK(!resolve_queries(nlohmann::json{{"queries", "not an array"}}, {"a"},
                           "ai-pulse", out).empty());
    CHECK(!resolve_queries(nlohmann::json{{"queries", nlohmann::json::array({""})}},
                           {"a"}, "ai-pulse", out).empty());
    CHECK(!resolve_queries(nlohmann::json{{"queries", nlohmann::json::array({1, 2})}},
                           {"a"}, "ai-pulse", out).empty());
    return 0;
}

// ── Dates ────────────────────────────────────────────────────────────────────

int test_iso_date() {
    CHECK(iso_date("2026-07-30") == "2026-07-30");
    CHECK(iso_date("2026-07-30T10:00:00Z") == "2026-07-30");
    CHECK(iso_date("Wed, 30 Jul 2026 10:00:00 GMT") == "2026-07-30");
    CHECK(iso_date("Jul 3, 2026") == "2026-07-03");
    CHECK(iso_date("3 July 2026") == "2026-07-03");

    // Unparseable is empty, never a guess: a wrong date silently drops a good
    // story or keeps a stale one.
    CHECK(iso_date("").empty());
    CHECK(iso_date("last Tuesday").empty());
    CHECK(iso_date("2026-13-40").empty());
    return 0;
}

int test_days_between() {
    CHECK(days_between("2026-07-30", "2026-07-31") == 1);
    CHECK(days_between("2026-07-31", "2026-07-30") == -1);
    CHECK(days_between("2026-07-31", "2026-07-31") == 0);
    // Across a month, a year and a leap day.
    CHECK(days_between("2026-07-31", "2026-08-01") == 1);
    CHECK(days_between("2025-12-31", "2026-01-01") == 1);
    CHECK(days_between("2024-02-28", "2024-03-01") == 2);
    return 0;
}

// ── Shortlisting ─────────────────────────────────────────────────────────────

static Filter today_filter() {
    Filter f;
    f.today = "2026-07-31";
    f.max_age_days = 2;
    return f;
}

int test_shortlist_dedups_urls() {
    std::vector<funes::tavily::Hit> hits = {
        hit("A story", "https://www.reuters.com/a/b"),
        hit("A story, syndicated", "https://reuters.com/a/b/?utm_source=feed"),
        hit("Another story", "https://reuters.com/a/c"),
    };
    std::vector<Rejected> rejected;
    auto out = shortlist(hits, today_filter(), &rejected);
    CHECK(out.size() == 2);
    CHECK(rejected.size() == 1);
    CHECK(rejected[0].reason == "duplicate url");
    return 0;
}

int test_shortlist_dedups_titles() {
    // Two outlets, one wire story. Different URLs, so only the title catches it.
    std::vector<funes::tavily::Hit> hits = {
        hit("OpenAI cuts prices", "https://a.com/1"),
        hit("OpenAI Cuts Prices!", "https://b.com/2"),
    };
    auto out = shortlist(hits, today_filter());
    CHECK(out.size() == 1);
    CHECK(out[0].source == "a.com");
    return 0;
}

int test_shortlist_drops_stale() {
    std::vector<funes::tavily::Hit> hits = {
        hit("Today", "https://a.com/1", "2026-07-31"),
        hit("Yesterday", "https://a.com/2", "2026-07-30"),
        hit("Last week", "https://a.com/3", "2026-07-24"),
        hit("No date given", "https://a.com/4", ""),
        // A date past the issue date is a timezone artifact, not staleness.
        hit("Tomorrow", "https://a.com/5", "2026-08-01"),
    };
    std::vector<Rejected> rejected;
    auto out = shortlist(hits, today_filter(), &rejected);
    CHECK(out.size() == 4);
    CHECK(rejected.size() == 1);
    CHECK(rejected[0].url == "https://a.com/3");
    CHECK(rejected[0].reason.find("older than 2 days") != std::string::npos);
    return 0;
}

int test_shortlist_drops_already_published() {
    Filter f = today_filter();
    // Stored canonical, matched against a hit that isn't.
    f.seen = {"reuters.com/a/b"};
    std::vector<funes::tavily::Hit> hits = {
        hit("Ran yesterday", "https://www.reuters.com/a/b?utm_source=x"),
        hit("New", "https://www.reuters.com/a/c"),
    };
    std::vector<Rejected> rejected;
    auto out = shortlist(hits, f, &rejected);
    CHECK(out.size() == 1);
    CHECK(out[0].url == "https://www.reuters.com/a/c");
    CHECK(rejected[0].reason == "ran in a recent issue");
    return 0;
}

int test_shortlist_caps_per_source() {
    Filter f = today_filter();
    f.max_per_source = 2;
    std::vector<funes::tavily::Hit> hits = {
        hit("One", "https://reuters.com/1"), hit("Two", "https://reuters.com/2"),
        hit("Three", "https://reuters.com/3"), hit("Four", "https://bbc.co.uk/4"),
    };
    auto out = shortlist(hits, f);
    CHECK(out.size() == 3);
    CHECK(out[2].source == "bbc.co.uk");

    // The cap is off by default, so the plain path is unaffected by it.
    CHECK(shortlist(hits, today_filter()).size() == 4);
    return 0;
}

int test_shortlist_preserves_search_rank() {
    // Search rank is the only quality signal available at this stage; sorting
    // by date instead would throw it away.
    std::vector<funes::tavily::Hit> hits = {
        hit("Older but ranked first", "https://a.com/1", "2026-07-30"),
        hit("Newer", "https://a.com/2", "2026-07-31"),
    };
    auto out = shortlist(hits, today_filter());
    CHECK(out.size() == 2);
    CHECK(out[0].title == "Older but ranked first");
    return 0;
}

int test_shortlist_assigns_no_ids() {
    // Ids come after the fetch stage, because a candidate can still die there
    // and a pool numbered 1,2,4 invites a model to ask for 3.
    auto out = shortlist({hit("A", "https://a.com/1")}, today_filter());
    CHECK(out.size() == 1);
    CHECK(out[0].id == 0);
    return 0;
}

// ── Story clustering ─────────────────────────────────────────────────────────
// The titles below are the ones the first live harvest actually returned on
// 2026-07-31: eight candidates, of which four were the same Anthropic
// disclosure under four different headlines.

static const char* kAnthropicNdtv =
    "Anthropic AI Models Hacked 3 Organisations During Cybersecurity Tests";
static const char* kAnthropicAbc =
    "Anthropic's Claude AI model hacks three companies during safety testing";
static const char* kAnthropicEuronews =
    "Anthropic admits its most powerful AI model hacked into three companies";
static const char* kEuLabels =
    "AI labels to be compulsory on authentic-looking content under EU rules";
static const char* kOpenSource =
    "Banning Open-Source AI Models to Protect Our Cybersecurity Makes No Sense";
static const char* kSecondCompany =
    "Second major AI company says its systems hacked into other firms";

int test_story_tokens() {
    const auto t = story_tokens("Anthropic's Claude AI Models Hacked 3 Companies");
    const auto has = [&](const std::string& w) {
        return std::find(t.begin(), t.end(), w) != t.end();
    };
    // "AI" is two characters, and in an AI newsletter it is in every headline
    // and separates nothing — dropping short tokens removes it for free.
    CHECK(!has("ai"));
    CHECK(has("anthropic"));
    CHECK(has("3"));
    // Stemmed to one token, so "hacked"/"hacks"/"hacking" agree.
    CHECK(has("hack"));
    CHECK(has("model"));
    CHECK(has("company"));

    // Spelled numbers and digits are the same story.
    CHECK(story_tokens("hacked three companies") ==
          story_tokens("hacks 3 company"));
    // Stop words carry no signal.
    CHECK(!has("the"));
    return 0;
}

int test_title_similarity_groups_one_story() {
    CHECK(title_similarity(kAnthropicNdtv, kAnthropicAbc) >= kStoryThreshold);
    CHECK(title_similarity(kAnthropicNdtv, kAnthropicEuronews) >= kStoryThreshold);
    CHECK(title_similarity(kAnthropicAbc, kAnthropicEuronews) >= kStoryThreshold);
    // Syndication: the identical headline on another site.
    CHECK(title_similarity(kAnthropicEuronews, kAnthropicEuronews) == 1.0);
    return 0;
}

int test_title_similarity_keeps_distinct_stories_apart() {
    // Merging two real stories costs an item, so these matter more than the
    // ones above.
    CHECK(title_similarity(kAnthropicNdtv, kEuLabels) < kStoryThreshold);
    CHECK(title_similarity(kAnthropicNdtv, kOpenSource) < kStoryThreshold);
    CHECK(title_similarity(kAnthropicNdtv, kSecondCompany) < kStoryThreshold);
    CHECK(title_similarity(kEuLabels, kOpenSource) < kStoryThreshold);

    // Two headlines sharing only generic vocabulary are not a story.
    CHECK(title_similarity("AI model released today", "AI model priced today") <
          kStoryThreshold);
    CHECK(title_similarity("", "anything") == 0.0);
    return 0;
}

int test_shortlist_caps_takes_on_one_story() {
    // The 2026-07-31 pool, in the order search returned it.
    std::vector<funes::tavily::Hit> hits = {
        hit(kAnthropicNdtv, "https://ndtv.com/1"),
        hit(kEuLabels, "https://theguardian.com/2"),
        hit(kSecondCompany, "https://washingtonpost.com/3"),
        hit(kAnthropicAbc, "https://abc.net.au/4"),
        hit(kOpenSource, "https://cnet.com/5"),
        hit(kAnthropicEuronews, "https://euronews.com/6"),
        hit(kAnthropicEuronews, "https://yahoo.com/7"),
    };

    std::vector<Rejected> rejected;
    auto out = shortlist(hits, today_filter(), &rejected);

    // Two takes on the Anthropic story survive — enough to choose an outlet —
    // and the other two are dropped so the pool covers more ground.
    CHECK(out.size() == 5);
    CHECK(rejected.size() == 2);
    CHECK(rejected[0].reason.find("takes on that story") != std::string::npos);

    // Every candidate is labelled, so the model can see the redundancy that is
    // left instead of running the same story twice.
    CHECK(out[0].story == out[3].story);            // ndtv and abc
    CHECK(out[0].story != out[1].story);            // ndtv and the EU story
    CHECK(out[1].story != out[2].story);
    return 0;
}

int test_shortlist_story_cap_can_be_turned_off() {
    Filter f = today_filter();
    f.max_per_story = 0;
    std::vector<funes::tavily::Hit> hits = {
        hit(kAnthropicNdtv, "https://ndtv.com/1"),
        hit(kAnthropicAbc, "https://abc.net.au/4"),
        hit(kAnthropicEuronews, "https://euronews.com/6"),
    };
    auto out = shortlist(hits, f);
    CHECK(out.size() == 3);
    // Still grouped, just not capped.
    CHECK(out[0].story == out[1].story && out[1].story == out[2].story);
    return 0;
}

int test_renumber_stories() {
    // What shortlisting leaves behind after the fetch stage has dropped
    // candidates: real groupings, arbitrary numbers.
    std::vector<Candidate> pool(5);
    pool[0].story = 2;
    pool[1].story = 7;
    pool[2].story = 2;      // same story as the first
    pool[3].story = 18;
    pool[4].story = 7;      // same story as the second

    renumber_stories(pool);
    CHECK(pool[0].story == 1);
    CHECK(pool[1].story == 2);
    CHECK(pool[2].story == 1);      // grouping survives
    CHECK(pool[3].story == 3);
    CHECK(pool[4].story == 2);

    std::vector<Candidate> empty;
    renumber_stories(empty);        // must not care
    return 0;
}

// ── Output ───────────────────────────────────────────────────────────────────

int test_excerpt() {
    const std::string short_text = "Short enough.";
    CHECK(excerpt(short_text, 600) == short_text);

    const std::string long_text = std::string(400, 'a') + " sentence one. sentence two "
                                  + std::string(400, 'b');
    const std::string cut = excerpt(long_text, 500);
    CHECK(cut.size() <= 505);
    CHECK(cut.rfind(" …") == cut.size() - 4);   // "…" is three bytes

    // A multi-byte character must never be cut in half — the excerpt is
    // JSON-serialized straight into the model's context.
    std::string accented;
    for (int i = 0; i < 400; ++i) accented += "é";
    const std::string sliced = excerpt(accented, 101);
    CHECK(sliced.size() <= 105);
    for (size_t i = 0; i < sliced.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(sliced[i]);
        if ((c & 0xE0) == 0xC0) CHECK(i + 1 < sliced.size());   // 2-byte lead has its tail
    }
    return 0;
}

int test_pool_for_model_hides_page_text() {
    // The pool has to fit in the context window; whole articles do not. The
    // full text is reachable through result_id and is in the pool file.
    Candidate c;
    c.id = 1;
    c.title = "A story";
    c.url = "https://a.com/1";
    c.source = "a.com";
    c.published = "2026-07-31";
    c.text = std::string(50000, 'x');
    c.result_id = 42;

    const auto j = pool_for_model("ai-pulse", "2026-07-31", {c}, 600);
    CHECK(j["publication"] == "ai-pulse");
    CHECK(j["candidates"].size() == 1);
    const auto& item = j["candidates"][0];
    CHECK(item["id"] == 1);
    CHECK(item["result_id"] == 42);
    CHECK(!item.contains("text"));
    CHECK(item["excerpt"].get<std::string>().size() < 700);
    CHECK(j.dump().size() < 2000);
    return 0;
}

int test_pool_record_keeps_page_text() {
    // The publisher checks quotes against this, so it must be complete.
    Candidate c;
    c.id = 1;
    c.url = "https://a.com/1";
    c.text = "the full extracted article text";
    const auto j = pool_record("ai-pulse", "2026-07-31", {c});
    CHECK(j["candidates"][0]["text"] == "the full extracted article text");
    CHECK(j["candidates"][0]["url"] == "https://a.com/1");
    return 0;
}

// ── Issue history ────────────────────────────────────────────────────────────

static void write_issue(const fs::path& dir, const std::string& name,
                        const std::string& body) {
    fs::create_directories(dir);
    std::ofstream(dir / name) << body;
}

int test_recently_published() {
    const fs::path dir = fs::temp_directory_path() / "funes_test_issues";
    fs::remove_all(dir);

    write_issue(dir, "2026-07-29.json", R"({"items":[{"url":"https://a.com/old"}]})");
    write_issue(dir, "2026-07-30.json", R"({"items":[{"url":"https://www.b.com/x/"}]})");
    write_issue(dir, "2026-07-31.json", R"({"items":[{"url":"https://c.com/y?utm_source=z"}]})");

    // Newest first, and stored canonical so a differently-decorated link to the
    // same article still matches.
    auto seen = recently_published(dir.string(), 2);
    CHECK(seen.size() == 2);
    CHECK(std::find(seen.begin(), seen.end(), "c.com/y") != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), "b.com/x") != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), "a.com/old") == seen.end());

    CHECK(recently_published(dir.string(), 7).size() == 3);
    CHECK(recently_published(dir.string(), 0).empty());

    // A publication with no history is the first run, not an error.
    CHECK(recently_published((dir / "nope").string(), 7).empty());

    // A corrupt issue file must not take the run down with it.
    write_issue(dir, "2026-08-01.json", "{ not json");
    CHECK(recently_published(dir.string(), 7).size() == 3);

    fs::remove_all(dir);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_strip_tracking();
    rc |= test_canonical_url();
    rc |= test_host_of();
    rc |= test_title_key();
    rc |= test_resolve_queries_omitted_key_uses_config();
    rc |= test_resolve_queries_empty_array_falls_back_to_config();
    rc |= test_resolve_queries_own_queries_win();
    rc |= test_resolve_queries_no_queries_anywhere_is_an_error();
    rc |= test_resolve_queries_caps_at_six();
    rc |= test_resolve_queries_rejects_the_wrong_type();
    rc |= test_iso_date();
    rc |= test_days_between();
    rc |= test_shortlist_dedups_urls();
    rc |= test_shortlist_dedups_titles();
    rc |= test_shortlist_drops_stale();
    rc |= test_shortlist_drops_already_published();
    rc |= test_shortlist_caps_per_source();
    rc |= test_shortlist_preserves_search_rank();
    rc |= test_shortlist_assigns_no_ids();
    rc |= test_story_tokens();
    rc |= test_title_similarity_groups_one_story();
    rc |= test_title_similarity_keeps_distinct_stories_apart();
    rc |= test_shortlist_caps_takes_on_one_story();
    rc |= test_shortlist_story_cap_can_be_turned_off();
    rc |= test_renumber_stories();
    rc |= test_excerpt();
    rc |= test_pool_for_model_hides_page_text();
    rc |= test_pool_record_keeps_page_text();
    rc |= test_recently_published();
    if (rc == 0) std::cout << "test_harvest: all tests passed\n";
    return rc;
}
