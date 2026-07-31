// =============================================================================
// src/core/tools/issue.cpp — publish_issue
// =============================================================================
// See issue.h for why the checks below are checks and not prompt sentences.

#include "issue.h"
#include "../publication.h"
#include "../text_utils.h"
#include "../tools.h"
#include "harvest.h"
#include "process_runner.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace funes::issue {
namespace {

// Typographic characters a model reproduces without being asked to. Each maps
// to the ASCII form the extracted page text is likely to carry.
const std::pair<const char*, const char*> kPunctuation[] = {
    {"‘", "'"}, {"’", "'"},                  // ' '
    {"“", "\""}, {"”", "\""},                // " "
    {"–", "-"}, {"—", "-"}, {"−", "-"}, // – — −
    {" ", " "},                                    // non-breaking space
};

// Both spellings of an elision, so a model that types three dots and one that
// types the character are treated the same.
const char* kEllipsis[] = {"…", "..."};

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
    return s.substr(a, b - a);
}

// Characters, not bytes — the limit that matters is what a reader sees, and a
// post full of emoji would otherwise be rejected for being two lines long.
size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string two(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

std::string now_iso() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    return std::to_string(tm.tm_year + 1900) + "-" + two(tm.tm_mon + 1) + "-" +
           two(tm.tm_mday) + "T" + two(tm.tm_hour) + ":" + two(tm.tm_min) + ":" +
           two(tm.tm_sec);
}

} // namespace

std::string normalize_for_match(const std::string& s) {
    std::string out = s;
    for (const auto& [from, to] : kPunctuation)
        out = replace_all(out, from, to);

    std::string folded;
    folded.reserve(out.size());
    bool in_space = true;                   // leading whitespace is dropped
    for (unsigned char c : out) {
        if (std::isspace(c)) {
            if (!in_space) folded += ' ';
            in_space = true;
        } else {
            folded += static_cast<char>(std::tolower(c));
            in_space = false;
        }
    }
    while (!folded.empty() && folded.back() == ' ') folded.pop_back();
    return folded;
}

std::string check_evidence(const std::string& quote, const std::string& page_text) {
    std::string normalized = normalize_for_match(quote);
    if (normalized.empty())
        return "no evidence quote was given";

    // Both elision spellings collapse to one marker before splitting.
    for (const char* e : kEllipsis)
        normalized = replace_all(normalized, e, "\x01");

    std::vector<std::string> fragments;
    size_t start = 0;
    while (start <= normalized.size()) {
        const size_t at = normalized.find('\x01', start);
        const std::string part = trim(normalized.substr(
            start, at == std::string::npos ? std::string::npos : at - start));
        if (!part.empty()) fragments.push_back(part);
        if (at == std::string::npos) break;
        start = at + 1;
    }

    if (fragments.empty())
        return "the evidence quote is only punctuation";
    if (fragments.size() > 2)
        return "the evidence quote has more than one … in it. One elision is "
               "allowed; quote a single continuous phrase instead";

    size_t total = 0;
    for (const auto& f : fragments) total += f.size();
    if (total < kMinEvidenceChars)
        return "the evidence quote is too short to prove anything (" +
               std::to_string(total) + " characters, need " +
               std::to_string(kMinEvidenceChars) + "). Quote a whole clause";

    const std::string haystack = normalize_for_match(page_text);
    size_t from = 0;
    for (const auto& fragment : fragments) {
        const size_t at = haystack.find(fragment, from);
        if (at == std::string::npos)
            return "this phrase does not appear on that page: \"" + fragment + "\"";
        from = at + fragment.size();
    }
    return {};
}

std::string headline_from_title(const std::string& title, size_t max_chars) {
    std::string clean = trim(title);
    // Outlets append their own name after a separator; the link already says
    // which site it points at.
    for (const char* sep : {" | ", " - ", " – ", " — "}) {
        const size_t at = clean.find(sep);
        if (at != std::string::npos && at >= 20) clean = clean.substr(0, at);
    }
    if (utf8_length(clean) <= max_chars) return clean;

    std::string cut = clean;
    funes::truncate_utf8_safe(cut, max_chars * 4);
    while (utf8_length(cut) > max_chars) cut.pop_back();
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
        cut.pop_back();
    const size_t space = cut.find_last_of(' ');
    if (space != std::string::npos && space > max_chars / 2) cut.resize(space);
    return trim(cut) + "…";
}

std::string build_issue(const nlohmann::json& pool,
                        const std::vector<Selection>& selection,
                        nlohmann::json& out) {
    if (!pool.contains("candidates") || !pool["candidates"].is_array())
        return "the candidate pool is unreadable — run harvest_candidates again";
    if (selection.empty())
        return "no items were given";

    std::map<int, int> used;               // candidate id → the item that took it
    nlohmann::json items = nlohmann::json::array();
    int n = 0;

    for (const auto& sel : selection) {
        ++n;
        const std::string where = "item " + std::to_string(n) + " (id " +
                                  std::to_string(sel.candidate_id) + "): ";

        const nlohmann::json* candidate = nullptr;
        for (const auto& c : pool["candidates"])
            if (c.value("id", 0) == sel.candidate_id) { candidate = &c; break; }
        if (!candidate) {
            // A model that just called read_result naturally reaches for the
            // same number here — but result_id (a global, ever-growing
            // counter into the result store) and a candidate's own `id` (small,
            // pool-local) are different things. Confusing them wasted an
            // attempt live on 2026-07-31: id 514 (in fact a result_id) matched
            // nothing, and the generic message alone didn't point at why. An
            // exact match against the pool's own result_ids lets the message
            // name the mistake and the fix in one line instead of two rounds.
            for (const auto& c : pool["candidates"]) {
                if (c.value("result_id", static_cast<int64_t>(0)) ==
                    static_cast<int64_t>(sel.candidate_id)) {
                    return where + "that is a result_id (from read_result), not a "
                                  "candidate id. The candidate you read has pool id " +
                                  std::to_string(c.value("id", 0)) + " — use that instead.";
                }
            }
            return where + "there is no candidate with that id in today's pool "
                           "(pool ids run 1 to " +
                   std::to_string(pool["candidates"].size()) +
                   "). Use only the small `id` field from the candidate list, "
                   "never a result_id";
        }
        const auto [taken, is_new] = used.emplace(sel.candidate_id, n);
        if (!is_new)
            return where + "that candidate is already item " +
                   std::to_string(taken->second) +
                   " — every item must be a different story";

        if (trim(sel.text).empty())
            return where + "the post text is empty";
        const size_t length = utf8_length(sel.text);
        if (length > kMaxPostChars)
            return where + "the post text is " + std::to_string(length) +
                   " characters; the limit is " + std::to_string(kMaxPostChars);
        if (trim(sel.emoji).empty())
            return where + "no emoji was given";

        const std::string problem = check_evidence(sel.evidence,
                                                   candidate->value("text", ""));
        if (!problem.empty())
            return where + problem + ". Quote a phrase copied exactly from that "
                   "candidate's excerpt, or from read_result on its result_id";

        items.push_back({
            {"n",            n},
            {"emoji",        trim(sel.emoji)},
            {"text",         trim(sel.text)},
            {"url",          candidate->value("url", "")},
            {"headline",     headline_from_title(candidate->value("title", ""))},
            {"source",       candidate->value("source", "")},
            {"published",    candidate->value("published", "")},
            {"evidence",     trim(sel.evidence)},
            {"candidate_id", sel.candidate_id}
        });
    }

    out = {
        {"publication", pool.value("publication", "")},
        {"date",        pool.value("date", "")},
        {"generated",   now_iso()},
        {"harvested",   pool["candidates"].size()},
        {"items",       items}
    };
    return {};
}

// ── The tool ─────────────────────────────────────────────────────────────────

namespace {

constexpr int    kPublishTimeoutSeconds = 300;
constexpr size_t kMaxScriptOutput       = 16 * 1024;

std::string safe_id(const std::string& raw) {
    std::string out;
    for (char c : raw)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            out += static_cast<char>(std::tolower(c));
    return out;
}

// A blocked run must leave something behind. A scheduled run that fails
// silently is indistinguishable from one that never started, and the whole
// point of a run record is that a human finds it the next morning.
void write_blocked_record(const std::filesystem::path& path,
                          const std::string& publication, const std::string& date,
                          const std::string& reason) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path);
    if (!out) return;
    out << funes::dump_safe(nlohmann::json{
        {"publication", publication},
        {"date",        date},
        {"status",      "blocked"},
        {"at",          now_iso()},
        {"reason",      reason}
    });
}

ToolResult publish_issue_handler(const std::string& default_workspace,
                                 const std::string& publishing_dir,
                                 const std::string& publications_dir,
                                 const json& args, const ToolContext& ctx) {
    if (!args.contains("items") || !args["items"].is_array())
        return {"Missing 'items': an array of {id, emoji, text, evidence}.", true};

    const std::string publication = safe_id(args.value("publication", "ai-pulse"));
    if (publication.empty())
        return {"'publication' must contain at least one letter or digit.", true};

    funes::Publication pub;
    const bool configured = funes::Publication::exists(publications_dir, publication);
    if (configured) {
        const std::string problem =
            funes::Publication::load(publications_dir, publication, pub);
        if (!problem.empty())
            return {"Publication '" + publication + "' is misconfigured: " + problem +
                    ". Nothing was sent.", true};
    }

    const std::filesystem::path workspace =
        ctx.workspace_dir.empty() ? default_workspace : ctx.workspace_dir;

    // The date comes from the pool, not from the model: the pool is the thing
    // being published, and a mismatch would publish yesterday's candidates
    // under today's headline.
    std::string date = args.value("date", "");
    if (date.empty()) {
        // Newest pool for this publication.
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(workspace, ec)) {
            const std::string name = e.path().filename().string();
            const std::string prefix = "harvest_" + publication + "_";
            if (name.rfind(prefix, 0) == 0 && e.path().extension() == ".json") {
                const std::string found = name.substr(prefix.size(), 10);
                if (found > date) date = found;
            }
        }
    }
    if (date.empty())
        return {"No candidate pool for '" + publication + "' in the workspace. "
                "Call harvest_candidates first — publishing resolves your ids "
                "against the pool it writes.", true};

    const std::filesystem::path pool_path =
        workspace / ("harvest_" + publication + "_" + date + ".json");
    nlohmann::json pool;
    {
        std::ifstream in(pool_path);
        if (!in)
            return {"No candidate pool at " + pool_path.filename().string() +
                    ". Call harvest_candidates first.", true};
        try { in >> pool; }
        catch (const std::exception& e) {
            return {std::string("The candidate pool is corrupt (") + e.what() +
                    "). Run harvest_candidates again.", true};
        }
    }

    std::vector<Selection> selection;
    for (const auto& item : args["items"]) {
        if (!item.is_object())
            return {"Every entry in 'items' must be an object with id, emoji, text "
                    "and evidence.", true};
        Selection sel;
        if (!item.contains("id") || !item["id"].is_number_integer())
            return {"Every item needs an integer 'id' from the candidate pool.", true};
        sel.candidate_id = item["id"].get<int>();
        sel.emoji        = item.value("emoji", "");
        sel.text         = item.value("text", "");
        sel.evidence     = item.value("evidence", "");
        selection.push_back(std::move(sel));
    }

    const std::filesystem::path run_path =
        workspace / "runs" / publication / (date + ".json");

    const int min_items = std::max(1, args.value("min_items", pub.min_count));
    if (static_cast<int>(selection.size()) < min_items) {
        const std::string why = "only " + std::to_string(selection.size()) +
                                " items selected; this publication needs at least " +
                                std::to_string(min_items);
        write_blocked_record(run_path, publication, date, why);
        return {"Not published: " + why + ". Pick more from the pool.", true};
    }

    nlohmann::json issue;
    const std::string problem = build_issue(pool, selection, issue);
    if (!problem.empty()) {
        write_blocked_record(run_path, publication, date, problem);
        return {"Not published — " + problem + ".\nNothing was sent. Fix that one "
                "item and call publish_issue again with the full list.", true};
    }

    if (configured) {
        // The scripts read their settings out of the issue rather than parsing
        // the config themselves: one YAML dialect, in one language, in one
        // place. The issue is already the source of truth for what goes out;
        // this makes it the source of truth for how, too.
        issue["title"]   = pub.title;
        issue["subject"] = pub.subject;
        if (!pub.cta.empty()) issue["cta"] = pub.cta;
        issue["artifacts"] = nlohmann::json::array();
        for (const auto& a : pub.artifacts)
            issue["artifacts"].push_back({{"kind", a.kind},
                                          {"path", a.path},
                                          {"template", a.html_template}});
        issue["channels"] = nlohmann::json::array();
        for (const auto& c : pub.channels)
            issue["channels"].push_back({{"kind", c.kind},
                                         {"recipients_file", c.recipients_file}});
    }

    const std::filesystem::path issue_path =
        workspace / "issues" / publication / (date + ".json");
    {
        std::error_code ec;
        std::filesystem::create_directories(issue_path.parent_path(), ec);
        std::ofstream out(issue_path);
        if (!out)
            return {"Could not write the issue at " + issue_path.string() +
                    " — nothing was sent.", true};
        out << funes::dump_safe(issue);
    }

    // Everything from here — rendering the artifacts, re-checking every link,
    // sending — is publishing/publish_issue.py, and its exit code is what makes
    // "it was sent" a fact rather than a claim.
    std::vector<std::string> argv = {
        "python3", (std::filesystem::path(publishing_dir) / "publish_issue.py").string(),
        "--dir", workspace.string(),
        "--publication", publication,
        date
    };
    if (args.value("send", true)) argv.push_back("--send");
    if (args.value("dry_run", false)) argv.push_back("--dry-run");

    const funes::proc::Result run =
        funes::proc::run_argv(argv, workspace, kPublishTimeoutSeconds, kMaxScriptOutput);

    if (run.timed_out) {
        write_blocked_record(run_path, publication, date, "publish_issue.py timed out");
        return {"publish_issue.py did not finish within " +
                std::to_string(kPublishTimeoutSeconds) + "s. Whether anything was "
                "sent is unknown — do not claim it was.", true};
    }
    if (run.exit_code != 0)
        return {"Not published (publish_issue.py exit " + std::to_string(run.exit_code) +
                "). Nothing was sent.\n" + run.output, true};

    return {"Published " + std::to_string(issue["items"].size()) + " items for " +
            publication + " " + date + ".\n" + run.output};
}

} // namespace
} // namespace funes::issue

void register_publish_issue_tool(ToolRegistry& reg,
                                 const std::string& workspace_dir,
                                 const std::string& publishing_dir,
                                 const std::string& publications_dir) {
    reg.add({
        "publish_issue",
        "Publish the day's issue from candidates you picked out of harvest_candidates. "
        "Give one entry per item: the candidate's `id`, an emoji, the post text, and an "
        "`evidence` quote copied verbatim from that candidate's page. "
        "You never supply a URL — each id is resolved against the pool, so the link that "
        "ships is the one that was fetched and checked. "
        "Every evidence quote is verified against the page it claims to come from; if one "
        "doesn't match, nothing is sent and you are told which item to fix. "
        "On success this has rendered the posts file and the newsletter, re-checked every "
        "link, and sent the mail.",
        {
            {"type", "object"},
            {"properties", {
                {"items", {
                    {"type", "array"},
                    {"minItems", 1},
                    {"description", "The selected items, in the order they should appear."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"id", {{"type", "integer"},
                                    {"description", "Candidate id from the pool"}}},
                            {"emoji", {{"type", "string"},
                                       {"description", "One emoji to open the post with"}}},
                            {"text", {{"type", "string"},
                                      {"description", "The post, under 280 characters, "
                                                      "written for X/LinkedIn"}}},
                            {"evidence", {{"type", "string"},
                                          {"description", "A phrase copied word for word "
                                                          "from that candidate's page, "
                                                          "supporting what the post claims. "
                                                          "One … may stand in for a gap."}}}
                        }},
                        {"required", json::array({"id", "emoji", "text", "evidence"})}
                    }}
                }},
                {"publication", {{"type", "string"},
                                 {"description", "Publication id (default 'ai-pulse')"}}},
                {"date", {{"type", "string"},
                          {"description", "Issue date YYYY-MM-DD (default: the newest pool)"}}},
                {"min_items", {{"type", "integer"},
                               {"description", "Refuse to publish fewer than this many items. "
                                               "Defaults to the publication's own minimum — "
                                               "you should not normally set this."}}},
                {"send", {{"type", "boolean"},
                          {"description", "Send the mail (default true). false renders and "
                                          "checks only."}}},
                {"dry_run", {{"type", "boolean"},
                             {"description", "Go through the send without delivering "
                                             "(default false)"}}}
            }},
            {"required", json::array({"items"})}
        },
        [workspace_dir, publishing_dir, publications_dir](const json& args,
                                                           const ToolContext& ctx) {
            return funes::issue::publish_issue_handler(workspace_dir, publishing_dir,
                                                       publications_dir, args, ctx);
        }
    });
}
