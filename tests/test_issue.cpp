// =============================================================================
// tests/test_issue.cpp — the checks that stand between a selection and a send
// =============================================================================
// Two properties are worth more than everything else here:
//
//   The URL that ships is never one the model typed. build_issue takes ids and
//   reads URLs out of the pool, so the test that matters is that a URL the
//   model supplies is ignored and the pool's is used.
//
//   Grounding. The system extracts a sentence from the candidate's page that
//   shares enough content words with the post to prove the post is about that
//   page. A post about topic X linked to a page about topic Y gets rejected
//   deterministically — no model involvement in the check at all.

#include "tools/issue.h"
#include <iostream>
#include <string>

using namespace funes::issue;
using nlohmann::json;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static const char* kPage =
    "Anthropic said its Claude model autonomously hacked into three "
    "organisations during a safety evaluation. The company disclosed the "
    "results on Thursday, weeks after OpenAI made a similar admission about "
    "its own systems. \"We were surprised by the capability,\" a spokesperson "
    "said. Regulators have asked for further detail.";

// ── Normalization ────────────────────────────────────────────────────────────

int test_normalize_for_match() {
    CHECK(normalize_for_match("  The   Quick\nBrown  ") == "the quick brown");
    CHECK(normalize_for_match("it\xe2\x80\x99s") == "it's");            // curly apostrophe
    CHECK(normalize_for_match("\xe2\x80\x9cquoted\xe2\x80\x9d") == "\"quoted\"");
    CHECK(normalize_for_match("a \xe2\x80\x94 b") == "a - b");          // em dash
    CHECK(normalize_for_match("a\xc2\xa0""b") == "a b");              // non-breaking space
    CHECK(normalize_for_match("").empty());
    return 0;
}

// ── Evidence extraction ─────────────────────────────────────────────────────

int test_extract_evidence_finds_matching_sentence() {
    // Post about Claude hacking should match the first sentence of kPage.
    auto [evidence, err] = extract_evidence(
        "Anthropic says Claude hacked three organisations during a safety test.",
        kPage);
    CHECK(err.empty());
    CHECK(!evidence.empty());
    CHECK(evidence.find("hacked") != std::string::npos ||
          evidence.find("Anthropic") != std::string::npos);
    return 0;
}

int test_extract_evidence_rejects_wrong_page() {
    // Post about a breach, page about Stripe checkout — zero topical overlap.
    const char* stripe_page = "Payment successful. Your subscription is active. "
                              "Manage billing in your account settings.";
    auto [evidence, err] = extract_evidence(
        "Anthropic says Claude hacked three organisations during a safety test.",
        stripe_page);
    CHECK(!err.empty());
    CHECK(err.find("does not appear to be about this page") != std::string::npos);
    CHECK(evidence.empty());
    return 0;
}

int test_extract_evidence_handles_empty_inputs() {
    auto [e1, err1] = extract_evidence("some post text", "");
    CHECK(!err1.empty());

    auto [e2, err2] = extract_evidence("", kPage);
    CHECK(!err2.empty());
    return 0;
}

int test_extract_evidence_picks_best_sentence() {
    const char* multi_page =
        "The weather in London was overcast with temperatures hovering around twelve degrees. "
        "Anthropic released a major update to Claude that significantly improves coding performance. "
        "Local football results showed three draws and two wins across the weekend fixtures.";
    auto [evidence, err] = extract_evidence(
        "Anthropic's Claude gets a big coding upgrade, improving developer experience.",
        multi_page);
    CHECK(err.empty());
    CHECK(evidence.find("Anthropic") != std::string::npos);
    CHECK(evidence.find("Claude") != std::string::npos);
    CHECK(evidence.find("weather") == std::string::npos);
    return 0;
}

int test_extract_evidence_ignores_nav_boilerplate() {
    // Nav items separated by \n get split into individual short fragments
    // that are filtered out, so the article sentence wins.
    const char* page_with_nav =
        "Add The New York Post on Google\n"
        "Ireland Politics\n"
        "Scotland\n"
        "Scotland Politics\n"
        "Wales\n"
        "Tech\n"
        "Business\n"
        "Meta released a new artificial intelligence model called Muse Glimmer "
        "that is small enough to run on consumer laptops and desktop computers.";
    auto [evidence, err] = extract_evidence(
        "Meta launches Muse Glimmer, a new open-weight AI model small enough "
        "to run on a Mac or PC.",
        page_with_nav);
    CHECK(err.empty());
    CHECK(evidence.find("Meta released") != std::string::npos);
    CHECK(evidence.find("Ireland") == std::string::npos);
    CHECK(evidence.find("Add The New York") == std::string::npos);
    return 0;
}

int test_extract_evidence_needs_minimum_overlap() {
    // Post shares only 1-2 content words with the page — not enough to be grounded.
    auto [evidence, err] = extract_evidence(
        "Apple launches a revolutionary new quantum computing chip for consumers.",
        kPage);
    CHECK(!err.empty());
    CHECK(evidence.empty());
    return 0;
}

// ── Headlines ────────────────────────────────────────────────────────────────

int test_headline_from_title() {
    CHECK(headline_from_title("Anthropic admits Claude hacked three firms") ==
          "Anthropic admits Claude hacked three firms");

    CHECK(headline_from_title("Anthropic admits Claude hacked three firms | Reuters") ==
          "Anthropic admits Claude hacked three firms");

    CHECK(headline_from_title("Post-mortem published") == "Post-mortem published");

    const std::string longt = headline_from_title(
        "A very long headline about artificial intelligence that runs well past "
        "any reasonable link label length", 40);
    CHECK(longt.size() <= 50);
    CHECK(longt.back() != ' ');
    return 0;
}

// ── build_issue ──────────────────────────────────────────────────────────────

static json pool_with_one_candidate() {
    return {
        {"publication", "ai-pulse"},
        {"date", "2026-07-31"},
        {"candidates", json::array({
            {{"id", 4},
             {"result_id", 514},
             {"title", "Anthropic admits Claude hacked three firms | Reuters"},
             {"url", "https://reuters.com/tech/anthropic-claude"},
             {"source", "reuters.com"},
             {"published", "2026-07-31"},
             {"text", kPage}}
        })}
    };
}

static Selection good_selection() {
    Selection s;
    s.candidate_id = 4;
    s.emoji = "\U0001F916";
    s.text = "Anthropic says Claude hacked three organisations during a safety test.";
    return s;
}

static const char* kUnrelatedPage =
    "OpenAI announced a new pricing tier for its enterprise API customers on "
    "Tuesday, cutting per-token costs by roughly thirty percent for high-volume "
    "users. The change takes effect next month and applies to existing contracts.";

// A second candidate whose page has nothing to do with good_selection()'s
// text, for testing the wrong-id-but-right-page substitution: id 7 is what a
// model might mistakenly declare while still writing accurately about id 4's
// actual story.
static json pool_with_two_candidates() {
    json p = pool_with_one_candidate();
    p["candidates"].push_back({
        {"id", 7},
        {"result_id", 515},
        {"title", "OpenAI cuts enterprise API pricing | PYMNTS"},
        {"url", "https://pymnts.com/openai-price-cuts"},
        {"source", "pymnts.com"},
        {"published", "2026-07-31"},
        {"text", kUnrelatedPage}
    });
    return p;
}

int test_build_issue_takes_the_url_from_the_pool() {
    json out;
    CHECK(build_issue(pool_with_one_candidate(), {good_selection()}, out).empty());
    CHECK(out["items"].size() == 1);
    const auto& item = out["items"][0];
    CHECK(item["url"] == "https://reuters.com/tech/anthropic-claude");
    CHECK(item["n"] == 1);
    CHECK(item["candidate_id"] == 4);
    CHECK(item["headline"] == "Anthropic admits Claude hacked three firms");
    CHECK(item["source"] == "reuters.com");
    CHECK(out["harvested"] == 1);
    // Evidence is auto-extracted, not model-provided.
    CHECK(item.contains("evidence"));
    CHECK(!item["evidence"].get<std::string>().empty());
    return 0;
}

int test_build_issue_ignores_any_url_the_model_supplies() {
    json out;
    CHECK(build_issue(pool_with_one_candidate(), {good_selection()}, out).empty());
    CHECK(out["items"][0]["url"].get<std::string>().find("reuters.com") !=
          std::string::npos);
    return 0;
}

int test_build_issue_rejects_an_unknown_id() {
    Selection s = good_selection();
    s.candidate_id = 99;
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {s}, out);
    CHECK(why.find("no candidate with that id") != std::string::npos);
    CHECK(why.find("99") != std::string::npos);
    CHECK(out.is_null());
    return 0;
}

int test_build_issue_names_a_result_id_mixup() {
    Selection s = good_selection();
    s.candidate_id = 514;
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {s}, out);
    CHECK(why.find("result_id") != std::string::npos);
    CHECK(why.find("pool id 4") != std::string::npos);
    CHECK(out.is_null());
    return 0;
}

int test_build_issue_rejects_a_repeated_id() {
    json out;
    const std::string why = build_issue(pool_with_one_candidate(),
                                        {good_selection(), good_selection()}, out);
    CHECK(why.find("already item 1") != std::string::npos);
    return 0;
}

int test_build_issue_rejects_bad_post_text() {
    json out;

    Selection empty = good_selection();
    empty.text = "   ";
    CHECK(build_issue(pool_with_one_candidate(), {empty}, out)
              .find("post text is empty") != std::string::npos);

    Selection too_long = good_selection();
    too_long.text = std::string(281, 'x');
    const std::string why = build_issue(pool_with_one_candidate(), {too_long}, out);
    CHECK(why.find("281 characters") != std::string::npos);

    // 280 emoji is 280 characters, not 1120 bytes' worth of rejection.
    Selection emoji_post = good_selection();
    for (int i = 0; i < 200; ++i) emoji_post.text += "\U0001F916";
    CHECK(build_issue(pool_with_one_candidate(), {emoji_post}, out)
              .find("200 characters") == std::string::npos);

    Selection no_emoji = good_selection();
    no_emoji.emoji = "";
    CHECK(build_issue(pool_with_one_candidate(), {no_emoji}, out)
              .find("no emoji") != std::string::npos);
    return 0;
}

int test_build_issue_rejects_ungrounded_post() {
    // Post about something completely different from the page.
    Selection s = good_selection();
    s.text = "Apple launches a revolutionary quantum computing chip for consumers.";
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {s}, out);
    CHECK(why.find("does not appear to be about this page") != std::string::npos);
    return 0;
}

// 2026-08-11: several live-run rejections were a model writing an accurate
// post but naming the wrong pool id for it — a content-matching mistake, not
// a writing one. build_issue now searches the rest of the pool for whichever
// unused candidate's page actually supports the text before giving up.
int test_build_issue_substitutes_a_candidate_that_actually_matches() {
    Selection s = good_selection();       // text is about the Claude hack (id 4's page)
    s.candidate_id = 7;                   // but declares OpenAI-pricing's id instead
    json out;
    const std::string why = build_issue(pool_with_two_candidates(), {s}, out);
    CHECK(why.empty());
    const auto& item = out["items"][0];
    CHECK(item["candidate_id"] == 4);
    CHECK(item["url"] == "https://reuters.com/tech/anthropic-claude");
    CHECK(item["source"] == "reuters.com");
    CHECK(!item["evidence"].get<std::string>().empty());
    return 0;
}

int test_build_issue_substitution_does_not_reuse_an_already_placed_candidate() {
    // Item 1 legitimately takes id 4. Item 2 declares the wrong id (7) for
    // text that only id 4's page actually supports — but 4 is spoken for, so
    // there is nothing left to substitute. The invariant under test is that
    // id 4 is NOT reused: two items pointing at one story is a duplicate, and
    // the whole reason substitution skips `used` candidates.
    //
    // What that costs item 2 changed with the drop behaviour: it used to sink
    // the entire issue, and now it is dropped and item 1 still ships. Same
    // invariant, survivable consequence.
    Selection first = good_selection();               // id 4, correct
    Selection second = good_selection();
    second.candidate_id = 7;
    json out;
    std::vector<std::string> dropped;
    const std::string why = build_issue(pool_with_two_candidates(), {first, second},
                                        out, /*min_items=*/1, &dropped);
    CHECK(why.empty());
    CHECK(out["items"].size() == 1);
    CHECK(out["items"][0]["url"] == "https://reuters.com/tech/anthropic-claude");
    CHECK(dropped.size() == 1);
    CHECK(dropped[0].find("item 2") != std::string::npos);
    return 0;
}

int test_build_issue_gives_up_when_no_candidate_matches_at_all() {
    // Wrong id, and nothing else in the pool matches the text either — a
    // real rejection, not a mismatch the pool happens to be able to fix.
    Selection s = good_selection();
    s.candidate_id = 7;
    s.text = "A completely unrelated claim about neither story in this pool.";
    json out;
    const std::string why = build_issue(pool_with_two_candidates(), {s}, out);
    CHECK(why.find("does not appear to be about this page") != std::string::npos);
    CHECK(out.is_null());
    return 0;
}

int test_build_issue_reports_only_bad_items() {
    Selection bad_second = good_selection();
    bad_second.candidate_id = 99;
    json out;
    const std::string why = build_issue(pool_with_one_candidate(),
                                        {good_selection(), bad_second}, out);
    CHECK(why.find("item 2") != std::string::npos);
    CHECK(why.find("item 1") == std::string::npos);
    return 0;
}

int test_build_issue_reports_all_violations() {
    Selection empty_text = good_selection();
    empty_text.text = "   ";
    Selection bad_id = good_selection();
    bad_id.candidate_id = 99;
    json out;
    const std::string why = build_issue(pool_with_one_candidate(),
                                        {empty_text, bad_id}, out);
    CHECK(why.find("item 1") != std::string::npos);
    CHECK(why.find("item 2") != std::string::npos);
    CHECK(why.find("post text is empty") != std::string::npos);
    CHECK(why.find("no candidate with that id") != std::string::npos);
    CHECK(out.is_null());
    return 0;
}

int test_build_issue_rejects_an_empty_selection() {
    json out;
    CHECK(!build_issue(pool_with_one_candidate(), {}, out).empty());
    CHECK(!build_issue(json::object(), {good_selection()}, out).empty());
    return 0;
}

// ── dropping an ungroundable item instead of losing the whole issue ──────────
// Before: one item nothing in the pool could support rejected the entire
// issue, and the model retried with identical arguments until the loop
// detector killed the run — three attempts, no newsletter. An item that
// cannot be grounded is now dropped and the rest ship, as long as enough
// survive.

static Selection grounded_on_id7() {
    Selection s;
    s.candidate_id = 7;
    s.emoji = "\U0001F4B0";
    s.text = "OpenAI cut enterprise API pricing for high-volume customers by thirty percent.";
    return s;
}

static Selection ungroundable_on_id7() {
    Selection s = grounded_on_id7();
    s.text = "A completely unrelated claim about neither story in this pool.";
    return s;
}

int test_build_issue_drops_an_ungroundable_item_and_keeps_the_rest() {
    json out;
    std::vector<std::string> dropped;
    const std::string why = build_issue(pool_with_two_candidates(),
                                        {good_selection(), ungroundable_on_id7()},
                                        out, /*min_items=*/1, &dropped);
    CHECK(why.empty());
    CHECK(out["items"].size() == 1);
    CHECK(out["items"][0]["url"] == "https://reuters.com/tech/anthropic-claude");
    // The drop is reported, not silent: a nine-item issue when ten were asked
    // for has to be visible to the caller and in the run record.
    CHECK(dropped.size() == 1);
    CHECK(dropped[0].find("item 2") != std::string::npos);
    return 0;
}

int test_build_issue_renumbers_after_a_drop() {
    // post_tweet.py indexes items 1..N, so a gap left by a dropped item would
    // make the cron post for that slot address the wrong story.
    json out;
    std::vector<std::string> dropped;
    const std::string why = build_issue(pool_with_two_candidates(),
                                        {ungroundable_on_id7(), good_selection()},
                                        out, /*min_items=*/1, &dropped);
    CHECK(why.empty());
    CHECK(out["items"].size() == 1);
    CHECK(out["items"][0]["n"] == 1);          // not 2
    CHECK(dropped.size() == 1);
    return 0;
}

int test_build_issue_rejects_when_too_few_survive() {
    // Dropping is not unlimited: below the publication's min_count there is no
    // issue worth sending, and the rejection has to say what was dropped and
    // why rather than leaving the model to guess.
    json out;
    std::vector<std::string> dropped;
    const std::string why = build_issue(pool_with_two_candidates(),
                                        {good_selection(), ungroundable_on_id7()},
                                        out, /*min_items=*/2, &dropped);
    CHECK(!why.empty());
    CHECK(out.is_null());
    CHECK(why.find("does not appear to be about this page") != std::string::npos);
    CHECK(why.find("item 2") != std::string::npos);
    return 0;
}

int test_build_issue_still_rejects_contract_violations() {
    // A duplicate id, an empty post, an over-long post: these are the model
    // getting the contract wrong, and it can fix them. They must still reject
    // the whole issue rather than being quietly dropped — otherwise a model
    // that submits ten malformed items gets a one-item newsletter.
    json out;
    std::vector<std::string> dropped;
    Selection dup = good_selection();          // same id as item 1
    const std::string why = build_issue(pool_with_two_candidates(),
                                        {good_selection(), dup},
                                        out, /*min_items=*/1, &dropped);
    CHECK(!why.empty());
    CHECK(out.is_null());
    CHECK(why.find("every item must be a different story") != std::string::npos);
    CHECK(dropped.empty());
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_normalize_for_match();
    rc |= test_extract_evidence_finds_matching_sentence();
    rc |= test_extract_evidence_rejects_wrong_page();
    rc |= test_extract_evidence_handles_empty_inputs();
    rc |= test_extract_evidence_picks_best_sentence();
    rc |= test_extract_evidence_ignores_nav_boilerplate();
    rc |= test_extract_evidence_needs_minimum_overlap();
    rc |= test_headline_from_title();
    rc |= test_build_issue_takes_the_url_from_the_pool();
    rc |= test_build_issue_ignores_any_url_the_model_supplies();
    rc |= test_build_issue_rejects_an_unknown_id();
    rc |= test_build_issue_names_a_result_id_mixup();
    rc |= test_build_issue_rejects_a_repeated_id();
    rc |= test_build_issue_rejects_bad_post_text();
    rc |= test_build_issue_rejects_ungrounded_post();
    rc |= test_build_issue_substitutes_a_candidate_that_actually_matches();
    rc |= test_build_issue_substitution_does_not_reuse_an_already_placed_candidate();
    rc |= test_build_issue_gives_up_when_no_candidate_matches_at_all();
    rc |= test_build_issue_reports_only_bad_items();
    rc |= test_build_issue_reports_all_violations();
    rc |= test_build_issue_rejects_an_empty_selection();
    rc |= test_build_issue_drops_an_ungroundable_item_and_keeps_the_rest();
    rc |= test_build_issue_renumbers_after_a_drop();
    rc |= test_build_issue_rejects_when_too_few_survive();
    rc |= test_build_issue_still_rejects_contract_violations();
    if (rc == 0) std::cout << "test_issue: all tests passed\n";
    return rc;
}
