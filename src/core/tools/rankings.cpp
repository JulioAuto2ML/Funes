// =============================================================================
// src/core/tools/rankings.cpp — merge_rankings
// =============================================================================
// A Borda count is arithmetic with one right answer, and `council-chair`'s
// prompt used to ask a local model to do it in-context over three JSON blobs
// it was also holding. Same class of mistake as the URL a model retyped from
// memory in the newsletter incident: the model is being asked for something a
// tool can supply exactly.
//
// Two pieces of it are not arithmetic, and both are why this is a tool rather
// than a formula in a prompt:
//
//   * **Matching.** Three panelists run independently, so they name the same
//     proposal three ways ("Audit Log", "audit-log", "audit log."). Matching
//     raw strings splits one proposal into three rows with a third of its
//     score each, which quietly hands the debate to a weaker idea. The
//     normalization here is deliberately crude — case, punctuation and
//     whitespace — because anything cleverer starts merging proposals that
//     really are different, and a wrong merge is invisible in the output.
//   * **Surviving the input.** `delegate_to_agent` returns a *string*, and a
//     panelist can fail. Accepting objects, JSON strings and fenced blocks,
//     and reporting what was unusable rather than failing on it, is what makes
//     "a 2-panelist debate is better than no debate" true in code instead of
//     in a sentence the model may or may not act on.

#include "../answer_schema.h"
#include "../tools.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace {

// Case-flattened, punctuation-stripped, whitespace-collapsed. Used only as a
// map key — every string shown to a human is the original.
std::string match_key(const std::string& raw) {
    std::string out;
    bool pending_space = false;
    for (unsigned char c : raw) {
        if (std::isalnum(c)) {
            if (pending_space && !out.empty()) out += ' ';
            pending_space = false;
            out += static_cast<char>(std::tolower(c));
        } else {
            pending_space = true;
        }
    }
    return out;
}

struct Tally {
    std::string label;        // first spelling seen, verbatim
    double      score  = 0;
    int         votes  = 0;
    int         best_rank = 0;
    std::string rationale;    // the highest-ranked panelist's reason
    size_t      first_seen = 0;
};

// One panelist's reply in any of the shapes one actually arrives in: the
// object itself, a bare array of proposals, or a string containing either.
bool as_proposals(const json& item, json& out) {
    json value = item;
    if (value.is_string()) {
        json parsed;
        if (!funes::extract_answer_json(value.get<std::string>(), parsed)) return false;
        value = std::move(parsed);
    }
    if (value.is_array()) { out = std::move(value); return !out.empty(); }
    if (value.is_object() && value.contains("proposals") && value["proposals"].is_array()) {
        out = value["proposals"];
        return !out.empty();
    }
    return false;
}

std::string perspective_of(const json& item, size_t index) {
    json value = item;
    if (value.is_string()) {
        json parsed;
        if (funes::extract_answer_json(value.get<std::string>(), parsed))
            value = std::move(parsed);
    }
    if (value.is_object() && value.contains("perspective") && value["perspective"].is_string())
        return value["perspective"].get<std::string>();
    return "panelist " + std::to_string(index + 1);
}

std::string title_of(const json& proposal) {
    for (const char* key : {"title", "proposal", "name"})
        if (proposal.contains(key) && proposal[key].is_string())
            return proposal[key].get<std::string>();
    if (proposal.is_string()) return proposal.get<std::string>();
    return {};
}

// The panelist's stated rank when it gave one, otherwise its position in the
// list. A model that returns proposals already in order but forgets the `rank`
// field has still ranked them.
int rank_of(const json& proposal, size_t position) {
    if (proposal.is_object() && proposal.contains("rank") &&
        proposal["rank"].is_number_integer()) {
        const int r = proposal["rank"].get<int>();
        if (r > 0) return r;
    }
    return static_cast<int>(position) + 1;
}

ToolResult merge_handler(const json& args, const ToolContext&) {
    if (!args.contains("rankings") || !args["rankings"].is_array() ||
        args["rankings"].empty())
        return {"Missing 'rankings': an array with one entry per panelist, each "
                "either the panelist's JSON object or the string it returned.", true};

    std::map<std::string, Tally> tallies;
    std::vector<std::string> skipped;
    int counted = 0;
    size_t order = 0;

    for (size_t i = 0; i < args["rankings"].size(); ++i) {
        json proposals;
        if (!as_proposals(args["rankings"][i], proposals)) {
            skipped.push_back(perspective_of(args["rankings"][i], i));
            continue;
        }
        ++counted;

        // Borda: with N proposals, rank 1 scores N, the last scores 1. Scored
        // per panelist, so a panelist who returned four proposals does not
        // outweigh one who returned three.
        const int n = static_cast<int>(proposals.size());
        for (size_t pos = 0; pos < proposals.size(); ++pos) {
            const json& p = proposals[pos];
            const std::string title = title_of(p);
            if (title.empty()) continue;
            const int rank = std::min(rank_of(p, pos), n);
            const double points = n - rank + 1;

            Tally& t = tallies[match_key(title)];
            if (t.votes == 0) {
                t.label      = title;
                t.first_seen = order++;
                t.best_rank  = rank;
            }
            t.score += points;
            t.votes += 1;
            if (rank <= t.best_rank) {
                t.best_rank = rank;
                if (p.is_object() && p.contains("rationale") && p["rationale"].is_string())
                    t.rationale = p["rationale"].get<std::string>();
            }
        }
    }

    if (counted == 0 || tallies.empty())
        return {"None of the " + std::to_string(args["rankings"].size()) +
                " entries in 'rankings' contained ranked proposals. Pass each "
                "panelist's reply through as it came back, or re-run the "
                "panelists — there is no ranking to merge.", true};

    std::vector<const Tally*> sorted;
    sorted.reserve(tallies.size());
    for (const auto& [_, t] : tallies) sorted.push_back(&t);
    // Score, then breadth of support, then the order the proposals were first
    // seen — deterministic, so the same three replies always merge the same way.
    std::sort(sorted.begin(), sorted.end(), [](const Tally* a, const Tally* b) {
        if (a->score != b->score) return a->score > b->score;
        if (a->votes != b->votes) return a->votes > b->votes;
        return a->first_seen < b->first_seen;
    });

    json ranking = json::array();
    for (const Tally* t : sorted) {
        json row = {{"proposal", t->label}, {"score", t->score}, {"votes", t->votes}};
        if (!t->rationale.empty()) row["rationale"] = t->rationale;
        ranking.push_back(std::move(row));
    }

    // A tie at the top is an outcome the chair has to report, not something to
    // resolve silently — the point of the debate is the decision, and "the
    // council split" is a decision a human may want to see.
    const bool tied = sorted.size() > 1 && sorted[0]->score == sorted[1]->score;

    json out = {{"method", "borda"},
                {"panelists", counted},
                {"merged_ranking", std::move(ranking)},
                {"winner", sorted[0]->label},
                {"tied", tied}};
    if (!skipped.empty()) out["skipped"] = skipped;
    return {out.dump(2), false};
}

} // namespace

void register_ranking_tools(ToolRegistry& reg) {
    reg.add({
        "merge_rankings",
        "Merge several ranked lists into one by Borda count, matching proposals "
        "that are named slightly differently and reporting any entry it could "
        "not read. Use it instead of adding up scores yourself — it returns the "
        "merged ranking, the winner, and whether the top is tied.",
        {{"type", "object"},
         {"properties", {
             {"rankings", {
                 {"type", "array"},
                 {"description", "One entry per ranked list. Each may be the "
                                 "object {perspective, proposals:[{rank,title,"
                                 "rationale}]}, a bare array of proposals, or the "
                                 "raw string a sub-agent returned."},
                 {"items", {{"type", json::array({"object", "array", "string"})}}}}}}},
         {"required", json::array({"rankings"})}},
        [](const json& args, const ToolContext& ctx) { return merge_handler(args, ctx); }
    });
}
