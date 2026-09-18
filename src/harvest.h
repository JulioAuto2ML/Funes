// =============================================================================
// src/core/tools/harvest.h — build a numbered candidate pool
// =============================================================================
// On 2026-07-31 the newsletter shipped an item about an OpenAI breach linked to
// a Stripe checkout page. It returned 200, so the link checker passed it.
//
// That was not a model being careless. It was an architecture that required a
// model to retype a URL it had read three steps earlier, from a transcript that
// had since been compressed, previewed and stored by reference. Every URL in
// the posts file was a fresh act of transcription; ask for ten a day and one
// comes out wrong every few weeks, forever, and no amount of prompt tightening
// changes that arithmetic.
//
// So the model stops being asked for URLs. harvest_candidates runs the queries,
// throws away duplicates and stale items, fetches each survivor (a fetch that
// fails drops the candidate here, before it is ever offered), and hands back a
// numbered pool. The model returns numbers. The URL it never saw is the URL
// that ships.
//
// Everything in this header is pure and tested directly; the tool that wires it
// to Tavily, the fetcher and the result store is at the bottom of harvest.cpp.

#pragma once
#include "json.hpp"
#include "tavily.h"
#include <string>
#include <vector>

namespace funes::harvest {

struct Candidate {
    int         id = 0;        // 1-based, stable for the session; what the model returns
    int         story = 0;     // 1-based group of candidates covering the same story
    std::string title;
    std::string url;           // as fetched, tracking parameters stripped
    std::string source;        // host, without "www."
    std::string published;     // YYYY-MM-DD; empty when the search didn't say
    std::string text;          // extracted page text, filled by the fetch stage
    int64_t     result_id = 0; // handle into the result store for the full text
};

// ── URL handling ─────────────────────────────────────────────────────────────

// Drops utm_*, fbclid, gclid and friends. Applied to the URL that is kept and
// published, not just to the dedup key: those parameters are someone else's
// analytics, and two links to the same article should look the same.
std::string strip_tracking(const std::string& url);

// The dedup key: scheme, "www.", a trailing slash and a #fragment removed, host
// lower-cased, tracking parameters gone. Not a URL — never fetch this.
std::string canonical_url(const std::string& url);

std::string host_of(const std::string& url);

// Lower-cased letters and digits only. Two outlets running the same wire story
// under "OpenAI Cuts Prices" and "OpenAI cuts prices!" collide here, which is
// the point.
std::string title_key(const std::string& title);

// ── Story clustering ─────────────────────────────────────────────────────────
// The first live harvest returned six outlets on one Anthropic disclosure out
// of eight candidates: a 25-candidate pool holding four distinct stories is a
// thin menu. title_key can't see it — the headlines genuinely differ — and a
// per-source cap is the wrong axis, since this is one story across many sources
// rather than many stories from one.

// A title's significant tokens: lower-cased, split on non-alphanumerics, tokens
// of 1-2 characters dropped (which is also what removes "AI" from every headline
// in an AI newsletter, where it carries no signal at all), stop words dropped,
// number words mapped to digits, and a crude suffix stem so "hacked", "hacks"
// and "hacking" are one token.
std::vector<std::string> story_tokens(const std::string& title);

// Overlap coefficient — |A∩B| / min(|A|,|B|) — rather than Jaccard, because a
// six-word headline and a twelve-word one about the same event should still
// match, and Jaccard punishes exactly that.
double title_similarity(const std::string& a, const std::string& b);

// Two titles are the same story at or above this, and only with at least
// kMinSharedTokens words in common. Deliberately reluctant: grouping two
// distinct stories costs a real item, while missing a pair only leaves the
// menu as thin as it was before.
constexpr double kStoryThreshold  = 0.5;
constexpr size_t kMinSharedTokens = 3;

// ── Dates ────────────────────────────────────────────────────────────────────

// YYYY-MM-DD out of whatever the search returned — ISO-8601, RFC-1123
// ("Wed, 30 Jul 2026 10:00:00 GMT"), or a bare date. Empty = unparseable.
std::string iso_date(const std::string& raw);

// Whole days from `from` to `to`, both YYYY-MM-DD. Undefined for invalid input;
// callers check with iso_date first.
int days_between(const std::string& from, const std::string& to);

// ── Query resolution ─────────────────────────────────────────────────────────
// A caller's own queries win when given; the publication's configured ones are
// the fallback. On 2026-07-31 this fell over both ways at once: the tool's own
// JSON schema marked `queries` required, so a model could never actually omit
// it as the description instructed — and when it reached for `[]` to mean "I
// have none of my own" (a reasonable reading of "optional"), that was rejected
// as an error too, forcing it to invent generic queries of its own instead of
// using the ones the publication was configured with.
//
// Empty return = `out` holds the resolved queries (capped at 6, never empty).
// Otherwise the message is written to be read by a model.
std::string resolve_queries(const nlohmann::json& args,
                            const std::vector<std::string>& configured,
                            const std::string& publication,
                            std::vector<std::string>& out);

// ── Shortlisting ─────────────────────────────────────────────────────────────

struct Filter {
    std::string today;                  // YYYY-MM-DD
    int         max_age_days   = 2;     // items published before this are dropped
    int         max_per_source = 0;     // 0 = no cap
    int         max_per_story  = 2;     // 0 = no cap; 2 leaves a choice of outlet
    std::vector<std::string> seen;      // canonical urls from recent issues
};

struct Rejected {
    std::string url;
    std::string reason;
};

// Dedup by URL and by title, drop stale items, cap per source. Arrival order is
// preserved, which makes the first query's results the strongest — search rank
// is the only quality signal available at this stage and throwing it away to
// sort by date would be a downgrade.
//
// Ids are deliberately NOT assigned here: a candidate can still die at the
// fetch stage, and a pool numbered 1,2,4,5 invites a model to ask for 3.
std::vector<Candidate> shortlist(const std::vector<tavily::Hit>& hits,
                                 const Filter& filter,
                                 std::vector<Rejected>* rejected = nullptr);

// Renumbers `story` densely from 1, in order of first appearance. Called after
// the fetch stage for the same reason ids are only assigned there: shortlisting
// counts stories the fetch then throws away, and a twelve-candidate pool whose
// highest story number is 18 reads as though six candidates went missing.
void renumber_stories(std::vector<Candidate>& candidates);

// ── Output ───────────────────────────────────────────────────────────────────

// UTF-8-safe head of `text`, backed up to the last sentence or word boundary so
// the model isn't reading a half-word.
std::string excerpt(const std::string& text, size_t max_bytes);

// What the model sees. Page text is deliberately absent — that is what the pool
// file and the result store are for — and so is anything the model would have
// to copy: it picks by `id`.
nlohmann::json pool_for_model(const std::string& publication,
                              const std::string& date,
                              const std::vector<Candidate>& candidates,
                              size_t excerpt_bytes);

// The pool file: everything, page text included, so the publisher can resolve
// an id to a URL and check a quote against the page it came from without
// trusting anything the model said.
nlohmann::json pool_record(const std::string& publication,
                           const std::string& date,
                           const std::vector<Candidate>& candidates);

// Canonical urls of items published in the last `issues` issue files under
// `issues_dir` (newest first by filename, which is the date). A missing
// directory is not an error — the first run of a publication has no history.
std::vector<std::string> recently_published(const std::string& issues_dir, int issues);

} // namespace funes::harvest

class ToolRegistry;
class MemoryStore;

// harvest_candidates. `workspace_dir` is where the pool file and the issue
// history live (ToolContext::workspace_dir overrides it per agent);
// `publications_dir` holds the publication configs, which supply the queries
// and windows when the caller doesn't.
void register_harvest_tool(ToolRegistry& reg, MemoryStore& memory,
                           const std::string& workspace_dir,
                           const std::string& publications_dir);
