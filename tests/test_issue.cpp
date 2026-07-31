// =============================================================================
// tests/test_issue.cpp — the checks that stand between a selection and a send
// =============================================================================
// Two properties are worth more than everything else here:
//
//   The URL that ships is never one the model typed. build_issue takes ids and
//   reads URLs out of the pool, so the test that matters is that a URL the
//   model supplies is ignored and the pool's is used.
//
//   A post cannot claim something its source doesn't say. That is check_evidence,
//   and it is tested from both directions: a real quote must survive the
//   normalization a model puts it through (case, curly quotes, line wrapping),
//   and a quote from the wrong page must not pass no matter how plausible it
//   sounds. A false reject costs one nudge; a false accept costs a wrong link
//   in a sent newsletter.

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
    CHECK(normalize_for_match("it’s") == "it's");            // curly apostrophe
    CHECK(normalize_for_match("“quoted”") == "\"quoted\"");
    CHECK(normalize_for_match("a — b") == "a - b");          // em dash
    CHECK(normalize_for_match("a b") == "a b");              // non-breaking space
    CHECK(normalize_for_match("").empty());
    return 0;
}

// ── Evidence ─────────────────────────────────────────────────────────────────

int test_evidence_accepts_a_real_quote() {
    CHECK(check_evidence("autonomously hacked into three organisations", kPage).empty());
    return 0;
}

int test_evidence_survives_a_models_retyping() {
    // Everything a model does to a phrase without meaning to.
    CHECK(check_evidence("Autonomously Hacked Into Three Organisations", kPage).empty());
    CHECK(check_evidence("autonomously hacked\n  into three organisations", kPage).empty());
    CHECK(check_evidence("“We were surprised by the capability,” a spokesperson",
                         kPage).empty());
    return 0;
}

int test_evidence_allows_one_elision() {
    CHECK(check_evidence("Anthropic said its Claude model … during a safety evaluation",
                         kPage).empty());
    CHECK(check_evidence("Anthropic said its Claude model ... during a safety evaluation",
                         kPage).empty());

    // Out of order is not evidence — it would let a model assemble a sentence
    // the page never made.
    CHECK(!check_evidence("during a safety evaluation … Anthropic said its Claude",
                          kPage).empty());

    // Two elisions is sentence construction, not quotation.
    const std::string two = check_evidence(
        "Anthropic said … hacked into … safety evaluation", kPage);
    CHECK(two.find("more than one") != std::string::npos);
    return 0;
}

int test_evidence_rejects_the_wrong_page() {
    // The 2026-07-31 failure: a post about a breach linked to a Stripe
    // checkout page. Plausible prose, wrong source, and no quote can bridge it.
    const char* stripe_page = "Payment successful. Your subscription is active. "
                              "Manage billing in your account settings.";
    const std::string why =
        check_evidence("autonomously hacked into three organisations", stripe_page);
    CHECK(!why.empty());
    CHECK(why.find("does not appear") != std::string::npos);
    return 0;
}

int test_evidence_rejects_a_paraphrase() {
    // Close enough to fool a reader, not a substring.
    CHECK(!check_evidence("Claude broke into three companies by itself", kPage).empty());
    return 0;
}

int test_evidence_rejects_the_trivially_true() {
    // "the" is on every page; a short quote proves nothing.
    const std::string why = check_evidence("the company", kPage);
    CHECK(why.find("too short") != std::string::npos);
    CHECK(check_evidence("", kPage).find("no evidence") != std::string::npos);
    CHECK(!check_evidence("… …", kPage).empty());
    return 0;
}

// ── Headlines ────────────────────────────────────────────────────────────────

int test_headline_from_title() {
    CHECK(headline_from_title("Anthropic admits Claude hacked three firms") ==
          "Anthropic admits Claude hacked three firms");

    // The outlet's own name is already visible in the link's host.
    CHECK(headline_from_title("Anthropic admits Claude hacked three firms | Reuters") ==
          "Anthropic admits Claude hacked three firms");

    // A hyphen inside a short title is part of the title, not a separator.
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
    s.evidence = "autonomously hacked into three organisations";
    return s;
}

int test_build_issue_takes_the_url_from_the_pool() {
    json out;
    CHECK(build_issue(pool_with_one_candidate(), {good_selection()}, out).empty());
    CHECK(out["items"].size() == 1);
    const auto& item = out["items"][0];
    CHECK(item["url"] == "https://reuters.com/tech/anthropic-claude");
    CHECK(item["n"] == 1);
    CHECK(item["candidate_id"] == 4);
    // Derived, not asked for.
    CHECK(item["headline"] == "Anthropic admits Claude hacked three firms");
    CHECK(item["source"] == "reuters.com");
    CHECK(out["harvested"] == 1);
    return 0;
}

int test_build_issue_ignores_any_url_the_model_supplies() {
    // Selection has no url field at all — this asserts the type, which is the
    // actual guarantee: there is no channel through which a typed URL can
    // reach the issue.
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
    CHECK(out.is_null());                       // nothing half-built escapes
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

int test_build_issue_rejects_unsupported_evidence() {
    Selection s = good_selection();
    s.evidence = "Anthropic denied everything and blamed a contractor";
    json out;
    const std::string why = build_issue(pool_with_one_candidate(), {s}, out);
    CHECK(why.find("item 1") != std::string::npos);
    CHECK(why.find("does not appear") != std::string::npos);
    // The recovery instruction has to name a tool the model actually has.
    CHECK(why.find("read_result") != std::string::npos);
    return 0;
}

int test_build_issue_reports_one_violation_at_a_time() {
    // Five complaints make a small model rewrite everything and break what was
    // already right, so the first failing item is the whole message.
    Selection bad_second = good_selection();
    bad_second.candidate_id = 99;
    json out;
    const std::string why = build_issue(pool_with_one_candidate(),
                                        {good_selection(), bad_second}, out);
    CHECK(why.rfind("item 2", 0) == 0);
    CHECK(why.find("item 1") == std::string::npos);
    return 0;
}

int test_build_issue_rejects_an_empty_selection() {
    json out;
    CHECK(!build_issue(pool_with_one_candidate(), {}, out).empty());
    CHECK(!build_issue(json::object(), {good_selection()}, out).empty());
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_normalize_for_match();
    rc |= test_evidence_accepts_a_real_quote();
    rc |= test_evidence_survives_a_models_retyping();
    rc |= test_evidence_allows_one_elision();
    rc |= test_evidence_rejects_the_wrong_page();
    rc |= test_evidence_rejects_a_paraphrase();
    rc |= test_evidence_rejects_the_trivially_true();
    rc |= test_headline_from_title();
    rc |= test_build_issue_takes_the_url_from_the_pool();
    rc |= test_build_issue_ignores_any_url_the_model_supplies();
    rc |= test_build_issue_rejects_an_unknown_id();
    rc |= test_build_issue_rejects_a_repeated_id();
    rc |= test_build_issue_rejects_bad_post_text();
    rc |= test_build_issue_rejects_unsupported_evidence();
    rc |= test_build_issue_reports_one_violation_at_a_time();
    rc |= test_build_issue_rejects_an_empty_selection();
    if (rc == 0) std::cout << "test_issue: all tests passed\n";
    return rc;
}
