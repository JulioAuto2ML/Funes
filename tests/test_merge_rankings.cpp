// =============================================================================
// tests/test_merge_rankings.cpp — Borda count belongs in code
// =============================================================================
// council-chair's system prompt used to say: "merge them using a Borda count:
// for N proposals, rank 1 gets N points… sum across all three panelists."
// That is arithmetic with one right answer, asked of a 7B model holding three
// JSON blobs in its context. This is the same arithmetic as a tool.
//
// The cases below are the ones the prose version got wrong in practice: three
// panelists who name the same proposal with different capitalisation, a
// panelist whose reply came back as a fenced string rather than an object, and
// a panelist who returned nothing at all — which the prompt did say to survive,
// and which the tool has to actually survive.

#include "tools.h"
#include <iostream>
#include <string>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static json panelist(const std::string& perspective,
                     const std::vector<std::string>& titles_in_rank_order) {
    json proposals = json::array();
    for (size_t i = 0; i < titles_in_rank_order.size(); ++i)
        proposals.push_back({{"rank", static_cast<int>(i) + 1},
                             {"title", titles_in_rank_order[i]},
                             {"rationale", "because"}});
    return {{"perspective", perspective}, {"proposals", proposals}};
}

// The tool answers with JSON and nothing else, so the chair can hand it
// straight to write_structured instead of transcribing it.
static json merged(const ToolResult& r) {
    json out = json::parse(r.text, nullptr, /*allow_exceptions=*/false);
    if (out.is_discarded()) {
        std::cerr << "  not JSON: " << r.text << "\n";
        return nullptr;
    }
    return out;
}

int test_borda_scores_are_the_arithmetic() {
    ToolRegistry reg;
    register_ranking_tools(reg);
    CHECK(reg.has("merge_rankings"));
    ToolContext ctx{"council-chair", "s1"};

    // Three panelists, three proposals each. Borda with N=3: 3/2/1 points.
    //   audit-log:  3 + 2 + 3 = 8
    //   on-prem:    2 + 3 + 1 = 6
    //   redaction:  1 + 1 + 2 = 4
    auto r = reg.call("merge_rankings", {{"rankings", json::array({
        panelist("builder", {"audit-log", "on-prem", "redaction"}),
        panelist("buyer",   {"on-prem", "audit-log", "redaction"}),
        panelist("critic",  {"audit-log", "redaction", "on-prem"})})}}, ctx);
    CHECK(!r.error);

    json m = merged(r);
    CHECK(m.is_object());
    CHECK(m["winner"] == "audit-log");
    CHECK(m["merged_ranking"].size() == 3);
    CHECK(m["merged_ranking"][0]["proposal"] == "audit-log");
    CHECK(m["merged_ranking"][0]["score"] == 8);
    CHECK(m["merged_ranking"][1]["proposal"] == "on-prem");
    CHECK(m["merged_ranking"][1]["score"] == 6);
    CHECK(m["merged_ranking"][2]["score"] == 4);
    CHECK(m["panelists"] == 3);
    return 0;
}

int test_the_same_proposal_named_three_ways_is_one_proposal() {
    ToolRegistry reg;
    register_ranking_tools(reg);
    ToolContext ctx{"council-chair", "s1"};

    // Panelists are independent runs, so they never agree on punctuation.
    // Matching on the raw string scatters one proposal across three rows with
    // a third of its score each — which is how a weak idea wins.
    auto r = reg.call("merge_rankings", {{"rankings", json::array({
        panelist("builder", {"Audit Log", "on-prem"}),
        panelist("buyer",   {"audit-log", "on-prem"}),
        panelist("critic",  {"  audit log.", "on-prem"})})}}, ctx);
    CHECK(!r.error);

    json m = merged(r);
    CHECK(m["merged_ranking"].size() == 2);
    CHECK(m["merged_ranking"][0]["score"] == 6);       // 2+2+2
    CHECK(m["merged_ranking"][0]["votes"] == 3);
    // The label kept is the first one seen verbatim, not the normalized key:
    // the normalization is for matching, and a human reads this record.
    CHECK(m["merged_ranking"][0]["proposal"] == "Audit Log");
    return 0;
}

int test_it_survives_what_panelists_actually_return() {
    ToolRegistry reg;
    register_ranking_tools(reg);
    ToolContext ctx{"council-chair", "s1"};

    // delegate_to_agent hands back a *string*. Requiring the chair to re-type
    // three JSON blobs into an argument is asking for the retyping error the
    // newsletter rewrite was about, so a string that contains JSON is fine.
    const std::string fenced =
        "```json\n" + panelist("buyer", {"on-prem", "audit-log"}).dump() + "\n```";

    auto r = reg.call("merge_rankings", {{"rankings", json::array({
        panelist("builder", {"audit-log", "on-prem"}),
        fenced,
        "the panelist timed out"})}}, ctx);
    CHECK(!r.error);

    json m = merged(r);
    // Two usable panelists, one unusable: a 2-panelist debate beats no debate,
    // and the record says which one was dropped rather than hiding it.
    CHECK(m["panelists"] == 2);
    CHECK(m["skipped"].size() == 1);
    CHECK(m["merged_ranking"][0]["score"] == 3);       // 2 + 1
    return 0;
}

int test_ties_and_refusals_are_explicit() {
    ToolRegistry reg;
    register_ranking_tools(reg);
    ToolContext ctx{"council-chair", "s1"};

    // A tie is a real outcome, not something to break by coin flip: equal
    // scores are reported as tied, ordered by how many panelists ranked them
    // and then alphabetically so the output is reproducible.
    auto tie = reg.call("merge_rankings", {{"rankings", json::array({
        panelist("builder", {"alpha", "beta"}),
        panelist("buyer",   {"beta", "alpha"})})}}, ctx);
    CHECK(!tie.error);
    json m = merged(tie);
    CHECK(m["merged_ranking"][0]["score"] == m["merged_ranking"][1]["score"]);
    CHECK(m["tied"] == true);
    CHECK(m["merged_ranking"][0]["proposal"] == "alpha");   // deterministic order

    // Nothing usable at all is an error the chair has to handle, not a
    // ranking of nothing that reads like a successful debate.
    auto none = reg.call("merge_rankings",
                         {{"rankings", json::array({"", "no idea"})}}, ctx);
    CHECK(none.error);

    CHECK(reg.call("merge_rankings", json::object(), ctx).error);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_borda_scores_are_the_arithmetic();
    rc |= test_the_same_proposal_named_three_ways_is_one_proposal();
    rc |= test_it_survives_what_panelists_actually_return();
    rc |= test_ties_and_refusals_are_explicit();
    if (rc == 0) std::cout << "test_merge_rankings: all passed\n";
    return rc;
}
