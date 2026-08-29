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

// A result_id names exactly one candidate, so it is accepted and resolved
// rather than sent back for correction. It used to be a rejection that told
// the model the right id for each item; on 2026-08-29 all eight items came
// back as result_ids, the model fixed all eight perfectly on the next call,
// and the round trip bought nothing but one more chance to run out of steps.
// read_result is what puts these numbers in front of it in the first place.
int test_build_issue_accepts_a_result_id() {
    Selection s = good_selection();
    s.candidate_id = 514;               // the pool's result_id for candidate 4
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {s}, out);
    CHECK(why.empty());
    CHECK(!out.is_null());
    CHECK(out["items"].size() == 1);
    // Resolved to the pool id, and the URL comes from that candidate — the
    // whole point is that the right story ships, not merely that nothing errored.
    CHECK(out["items"][0]["candidate_id"] == 4);
    CHECK(out["items"][0]["url"] == "https://reuters.com/tech/anthropic-claude");
    return 0;
}

// Naming one candidate twice — once by pool id, once by its result_id — is
// still a duplicate. The dedup key has to be the *resolved* id or this slips
// through and the issue runs the same story twice.
int test_build_issue_catches_a_pool_id_and_result_id_duplicate() {
    Selection a = good_selection();          // id 4
    Selection b = good_selection();
    b.candidate_id = 514;                    // same candidate, other number
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {a, b}, out);
    CHECK(why.find("already item 1") != std::string::npos);
    CHECK(out.is_null());
    return 0;
}

// An id that is neither a pool id nor a result_id is still an error.
int test_build_issue_rejects_a_number_that_is_neither() {
    Selection s = good_selection();
    s.candidate_id = 9999;
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {s}, out);
    CHECK(why.find("no candidate with that id") != std::string::npos);
    CHECK(out.is_null());
    return 0;
}

static std::string repeat_to_length(const std::string& sentence, size_t times) {
    std::string out;
    for (size_t i = 0; i < times; ++i) out += sentence;
    return out;
}

int test_shorten_post() {
    // Already short: returned untouched, no ellipsis bolted on.
    const std::string ok = "Anthropic says Claude hacked three organisations.";
    CHECK(funes::issue::shorten_post(ok, 280) == ok);

    // Cuts at a sentence boundary when there is a usable one, and the result
    // reads as a finished thought rather than a truncation.
    const std::string two = "Anthropic says Claude hacked three organisations during a "
                            "safety test that ran for several weeks. " +
                            repeat_to_length("The report goes into considerable detail. ", 6);
    const std::string cut = funes::issue::shorten_post(two, 140);
    CHECK(funes::issue::utf8_length(cut) <= 140);
    CHECK(cut.back() == '.');
    CHECK(cut.find("\u2026") == std::string::npos);

    // No sentence boundary in range: word boundary plus an ellipsis, and the
    // ellipsis must not push it back over the limit.
    const std::string one_long = repeat_to_length("word ", 100);
    const std::string cut2 = funes::issue::shorten_post(one_long, 60);
    CHECK(funes::issue::utf8_length(cut2) <= 60);
    CHECK(cut2.size() >= 3);
    return 0;
}

// The failure of 2026-08-29: told "329 characters; the limit is 280", the model
// resubmitted byte-identical text and the loop detector killed the run. The
// first rejection now carries a ready-made replacement, because in that same
// run the model corrected eight wrong ids flawlessly when handed the values.
int test_build_issue_suggests_a_shorter_post() {
    Selection s = good_selection();
    s.text = "Anthropic says Claude hacked three organisations during a safety test. " +
             repeat_to_length("The company published a long and detailed report. ", 6);
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {s}, out);
    CHECK(why.find("the limit is 280") != std::string::npos);
    CHECK(out.is_null());

    // The suggestion is present, and is itself publishable — a suggestion over
    // the limit would send the model round the same loop.
    const std::string marker = "instead, or write your own shorter one: ";
    const size_t at = why.find(marker);
    CHECK(at != std::string::npos);
    const std::string suggested = why.substr(at + marker.size());
    CHECK(!suggested.empty());
    CHECK(funes::issue::utf8_length(suggested) <= 280);
    return 0;
}

// Second attempt: trim rather than lose the issue.
int test_build_issue_trims_on_the_second_attempt() {
    Selection s = good_selection();
    s.text = "Anthropic says Claude hacked three organisations during a safety test. " +
             repeat_to_length("The company published a long and detailed report. ", 6);
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {s}, out, 1, nullptr,
                                        /*trim_over_length=*/true);
    CHECK(why.empty());
    CHECK(!out.is_null());
    const std::string published = out["items"][0]["text"].get<std::string>();
    CHECK(funes::issue::utf8_length(published) <= 280);
    // The trimmed text is what ships — not the original, and not something
    // that lost the story: it still has to be grounded in the candidate page,
    // which is checked against the trimmed version.
    CHECK(published.find("Anthropic") != std::string::npos);
    CHECK(!out["items"][0]["evidence"].get<std::string>().empty());
    return 0;
}

// Far past the limit is the wrong text in the field, not a long post. Trimming
// would publish a stub with a link, so it is refused even on a second attempt.
int test_build_issue_refuses_to_trim_an_article() {
    Selection s = good_selection();
    s.text = repeat_to_length("Anthropic says Claude hacked three organisations. ", 40);
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {s}, out, 1, nullptr,
                                        /*trim_over_length=*/true);
    CHECK(why.find("far past the 280 limit") != std::string::npos);
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

// ── which pool gets published ────────────────────────────────────────────────

int test_pool_date_uses_today_when_todays_pool_exists() {
    auto c = resolve_pool_date({"2026-08-24", "2026-08-25", "2026-08-26"}, "", "2026-08-26");
    CHECK(c.error.empty());
    CHECK(c.date == "2026-08-26");
    return 0;
}

int test_pool_date_refuses_to_fall_back_to_a_stale_pool() {
    // The 2026-08-26 incident: harvest failed that morning, so the newest pool
    // on disk was the previous day's. Publishing against it grounded today's
    // posts in yesterday's pages. Refuse, and say what to do.
    auto c = resolve_pool_date({"2026-08-24", "2026-08-25"}, "", "2026-08-26");
    CHECK(c.date.empty());
    CHECK(c.error.find("2026-08-26") != std::string::npos);   // what is missing
    CHECK(c.error.find("2026-08-25") != std::string::npos);   // what exists instead
    CHECK(c.error.find("harvest_candidates") != std::string::npos);
    return 0;
}

int test_pool_date_honours_an_explicit_older_date() {
    // Republishing an old day on purpose stays possible — the ids are then
    // resolved against the pool that day actually used.
    auto c = resolve_pool_date({"2026-08-24", "2026-08-25"}, "2026-08-24", "2026-08-26");
    CHECK(c.error.empty());
    CHECK(c.date == "2026-08-24");
    return 0;
}

int test_pool_date_rejects_an_explicit_date_with_no_pool() {
    auto c = resolve_pool_date({"2026-08-24"}, "2026-08-25", "2026-08-26");
    CHECK(c.date.empty());
    CHECK(c.error.find("2026-08-25") != std::string::npos);
    return 0;
}

int test_pool_date_rejects_an_empty_workspace() {
    auto c = resolve_pool_date({}, "", "2026-08-26");
    CHECK(c.date.empty());
    CHECK(!c.error.empty());
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
    rc |= test_build_issue_accepts_a_result_id();
    rc |= test_build_issue_catches_a_pool_id_and_result_id_duplicate();
    rc |= test_build_issue_rejects_a_number_that_is_neither();
    rc |= test_shorten_post();
    rc |= test_build_issue_suggests_a_shorter_post();
    rc |= test_build_issue_trims_on_the_second_attempt();
    rc |= test_build_issue_refuses_to_trim_an_article();
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
    rc |= test_pool_date_uses_today_when_todays_pool_exists();
    rc |= test_pool_date_refuses_to_fall_back_to_a_stale_pool();
    rc |= test_pool_date_honours_an_explicit_older_date();
    rc |= test_pool_date_rejects_an_explicit_date_with_no_pool();
    rc |= test_pool_date_rejects_an_empty_workspace();
    if (rc == 0) std::cout << "test_issue: all tests passed\n";
    return rc;
}
