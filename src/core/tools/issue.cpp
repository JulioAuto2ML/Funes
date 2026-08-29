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
#include <iostream>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>

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

std::string two(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

// Local time, matching harvest.cpp's today_local(): the two have to agree on
// what "today" is or publish_issue would look for a pool harvest never named.
std::string today_iso() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    return std::to_string(tm.tm_year + 1900) + "-" + two(tm.tm_mon + 1) + "-" +
           two(tm.tm_mday);
}

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

// ── Stop words: common English words that carry no topical signal ────────────
const std::unordered_set<std::string> kStopWords = {
    "the", "a", "an", "and", "or", "but", "in", "on", "at", "to", "for",
    "of", "is", "are", "was", "were", "be", "been", "has", "have", "had",
    "will", "can", "its", "this", "that", "with", "from", "by", "not",
    "also", "than", "more", "most", "into", "over", "just", "about",
    "after", "before", "between", "through", "under", "such", "very",
    "only", "even", "both", "each", "all", "any", "some", "new", "first",
    "last", "other", "same", "which", "their", "them", "they", "what",
    "when", "where", "how", "who", "would", "could", "should", "does",
    "did", "being", "here", "there", "then", "now", "out", "own", "these",
    "those", "said", "says", "one", "two", "may", "like", "well", "use",
    "used", "using", "make", "made", "get", "got", "set", "way", "many"
};

constexpr size_t kMinWordLen = 4;

std::set<std::string> content_words(const std::string& text) {
    std::set<std::string> words;
    std::string word;
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            word += static_cast<char>(std::tolower(c));
        } else if (!word.empty()) {
            if (word.size() >= kMinWordLen && kStopWords.find(word) == kStopWords.end())
                words.insert(word);
            word.clear();
        }
    }
    if (word.size() >= kMinWordLen && kStopWords.find(word) == kStopWords.end())
        words.insert(word);
    return words;
}

constexpr size_t kMinSentenceChars = 40;

std::vector<std::string> split_sentences(const std::string& text) {
    std::vector<std::string> sentences;
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        current += text[i];
        bool is_break = false;
        if (text[i] == '.' || text[i] == '!' || text[i] == '?') {
            if (i + 1 >= text.size() || std::isspace(static_cast<unsigned char>(text[i + 1])))
                is_break = true;
        }
        // Every newline is a structural break in extracted web text — it marks
        // a different HTML element. Without this, nav items ("Ireland\nScotland
        // \nWales\n...") get concatenated into one blob that scores well because
        // real article text is buried at the end of it.
        if (text[i] == '\n')
            is_break = true;
        if (is_break) {
            std::string s = trim(current);
            if (s.size() >= kMinSentenceChars) sentences.push_back(std::move(s));
            current.clear();
        }
    }
    std::string s = trim(current);
    if (s.size() >= kMinSentenceChars) sentences.push_back(std::move(s));
    return sentences;
}

std::pair<std::string, std::string> extract_evidence(
    const std::string& post_text, const std::string& page_text) {
    if (page_text.empty())
        return {"", "the candidate's page text is empty"};

    const auto post_words = content_words(post_text);
    if (post_words.empty())
        return {"", "the post text has no content words to match against"};

    auto sentences = split_sentences(page_text);
    if (sentences.empty())
        return {"", "the candidate's page has no extractable sentences"};

    std::string best;
    int best_score = 0;
    for (const auto& sentence : sentences) {
        const auto sent_words = content_words(sentence);
        int overlap = 0;
        for (const auto& w : post_words)
            if (sent_words.count(w)) ++overlap;
        if (overlap > best_score) {
            best_score = overlap;
            best = sentence;
        }
    }

    if (best_score < kMinOverlap)
        return {"", "the post does not appear to be about this page — the best "
                    "sentence shares only " + std::to_string(best_score) +
                    " content word(s) with the post (need " +
                    std::to_string(kMinOverlap) + ")"};

    return {best, ""};
}

PoolChoice resolve_pool_date(const std::vector<std::string>& available_dates,
                             const std::string& requested,
                             const std::string& today) {
    std::string newest;
    for (const auto& d : available_dates)
        if (d > newest) newest = d;

    if (available_dates.empty())
        return {"", "there is no candidate pool in the workspace at all. Call "
                    "harvest_candidates first — publishing resolves your ids "
                    "against the pool it writes."};

    if (!requested.empty()) {
        for (const auto& d : available_dates)
            if (d == requested) return {requested, ""};
        return {"", "no candidate pool for " + requested + " (the newest is " +
                    newest + "). Call harvest_candidates for that date, or "
                    "publish the day a pool exists for."};
    }

    for (const auto& d : available_dates)
        if (d == today) return {today, ""};

    // Deliberately NOT falling back to `newest`. See the header: this is the
    // path that published today's text against yesterday's pages.
    return {"", "no candidate pool for today (" + today + "); the newest is " +
                newest + ". harvest_candidates has not run successfully today, "
                "so there is nothing to publish. Run it, and only pass "
                "date=" + newest + " if you really mean to republish that day."};
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

size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Which publication+date have already had a post rejected for length. A
// function-local static rather than a global so there is no initialization
// order to reason about, and so the mutex and the set cannot be separated.
static std::set<std::string>& length_retries() {
    static std::set<std::string> s;
    return s;
}
static std::mutex& length_retry_mu() {
    static std::mutex m;
    return m;
}

std::string shorten_post(const std::string& text, size_t max_chars) {
    std::string clean = trim(text);
    if (utf8_length(clean) <= max_chars) return clean;

    // Leave room for the ellipsis up front, so the word-boundary branch cannot
    // land back over the limit by adding it.
    const size_t budget = max_chars > 1 ? max_chars - 1 : max_chars;

    std::string cut = clean;
    funes::truncate_utf8_safe(cut, budget * 4);
    while (utf8_length(cut) > budget) cut.pop_back();
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
        cut.pop_back();

    // A sentence boundary is the only cut that reads as deliberate. Require it
    // to be past the halfway mark, or "trimming" a long first sentence would
    // throw away most of the post.
    const size_t floor = utf8_length(cut) / 2;
    size_t best = std::string::npos;
    for (size_t i = 0; i < cut.size(); ++i) {
        if (cut[i] != '.' && cut[i] != '!' && cut[i] != '?') continue;
        // Not a decimal point or an ellipsis mid-sentence.
        if (i + 1 < cut.size() && cut[i + 1] != ' ') continue;
        if (utf8_length(cut.substr(0, i + 1)) < floor) continue;
        best = i + 1;
    }
    if (best != std::string::npos) return trim(cut.substr(0, best));

    const size_t space = cut.find_last_of(' ');
    if (space != std::string::npos && utf8_length(cut.substr(0, space)) >= floor)
        cut.resize(space);
    return trim(cut) + "…";
}

std::string build_issue(const nlohmann::json& pool,
                        const std::vector<Selection>& selection,
                        nlohmann::json& out,
                        int min_items,
                        std::vector<std::string>* dropped,
                        bool trim_over_length) {
    if (!pool.contains("candidates") || !pool["candidates"].is_array())
        return "the candidate pool is unreadable — run harvest_candidates again";
    if (selection.empty())
        return "no items were given";

    std::map<int, int> used;               // candidate id → the item that took it
    nlohmann::json items = nlohmann::json::array();
    std::vector<std::string> errors;
    // Always collected, whether or not the caller asked for them: a rejection
    // has to carry the reasons even when `dropped` is null.
    std::vector<std::string> drops;
    int n = 0;

    for (const auto& sel : selection) {
        ++n;
        const std::string where = "item " + std::to_string(n) + " (id " +
                                  std::to_string(sel.candidate_id) + "): ";

        // Pool id first, always: if a number is both a valid pool id and some
        // other candidate's result_id, the pool id is what was meant.
        const nlohmann::json* candidate = nullptr;
        int effective_id = sel.candidate_id;
        for (const auto& c : pool["candidates"])
            if (c.value("id", 0) == sel.candidate_id) { candidate = &c; break; }
        if (!candidate) {
            // A result_id names exactly one candidate in this pool, so the
            // intent is unambiguous — accept it and carry on rather than
            // spending a round trip telling the model a number it already
            // gave us. This used to be a rejection listing the right id for
            // each item; on 2026-08-29 all eight items came back as
            // result_ids, the model applied all eight corrections perfectly,
            // and the only thing the round trip bought was one more chance to
            // run out of steps. read_result is what puts these numbers in
            // front of it in the first place.
            for (const auto& c : pool["candidates"]) {
                if (c.value("result_id", static_cast<int64_t>(0)) ==
                    static_cast<int64_t>(sel.candidate_id)) {
                    candidate = &c;
                    effective_id = c.value("id", 0);
                    std::cerr << "[publish_issue] item " << n << ": id "
                              << sel.candidate_id << " is a result_id; using pool id "
                              << effective_id << "\n";
                    break;
                }
            }
        }
        if (!candidate) {
            errors.push_back(where + "there is no candidate with that id in today's pool "
                           "(pool ids run 1 to " +
                   std::to_string(pool["candidates"].size()) +
                   "). Use only the small `id` field from the candidate list");
            continue;
        }
        // Keyed on the resolved id, so naming one candidate twice — once by
        // pool id and once by result_id — is still caught as a duplicate.
        const auto [taken, is_new] = used.emplace(effective_id, n);
        if (!is_new) {
            errors.push_back(where + "that candidate is already item " +
                   std::to_string(taken->second) +
                   " — every item must be a different story");
            continue;
        }

        if (trim(sel.text).empty()) {
            errors.push_back(where + "the post text is empty");
            continue;
        }
        // Over-length is the one violation the model has proved it cannot fix
        // from a description (see the header). So: name the problem *and* hand
        // it the fixed text, and if it comes back over the limit again, apply
        // that text ourselves rather than lose the issue.
        std::string post_text = sel.text;
        const size_t length = utf8_length(post_text);
        if (length > kMaxPostChars) {
            if (length > kMaxTrimmablePostChars) {
                // Far past the limit is not a long post, it is the wrong text
                // in the field. Trimming would publish a stub, so refuse.
                errors.push_back(where + "the post text is " + std::to_string(length) +
                       " characters, far past the " + std::to_string(kMaxPostChars) +
                       " limit — that looks like the article rather than a post. "
                       "Write one or two sentences about this story.");
                continue;
            }
            const std::string shorter = shorten_post(post_text, kMaxPostChars);
            if (!trim_over_length) {
                errors.push_back(where + "the post text is " + std::to_string(length) +
                       " characters; the limit is " + std::to_string(kMaxPostChars) +
                       ". Use exactly this text instead, or write your own shorter "
                       "one: " + shorter);
                continue;
            }
            std::cerr << "[publish_issue] item " << n << " (id " << effective_id
                      << "): post text was " << length << " characters after a second "
                         "attempt; trimmed to " << utf8_length(shorter) << "\n";
            post_text = shorter;
        }
        if (trim(sel.emoji).empty()) {
            errors.push_back(where + "no emoji was given");
            continue;
        }

        // post_text, not sel.text: if it was trimmed above, the trimmed
        // version is what ships, so that is what has to be grounded.
        auto extraction = extract_evidence(post_text, funes::json_string(*candidate, "text"));
        std::string evidence = extraction.first;
        std::string problem = extraction.second;

        if (!problem.empty()) {
            // The declared candidate's page doesn't support this text. Before
            // rejecting, check whether some OTHER unused candidate in the
            // pool does — a model that wrote an accurate post but named the
            // wrong id for it is a content-matching mistake, not a writing
            // one, and exactly as fixable in code as a broken link is (see
            // publish_issue.py's repair_and_check, the same idea one stage
            // later for a link that dies between here and send time). Not
            // restricted to the same `story`: the id is simply wrong, not
            // usefully "close."
            const nlohmann::json* substitute = nullptr;
            std::string substitute_evidence;
            for (const auto& c : pool["candidates"]) {
                const int cid = c.value("id", 0);
                if (cid == effective_id || used.count(cid)) continue;
                auto alt = extract_evidence(post_text, funes::json_string(c, "text"));
                if (alt.second.empty()) {
                    substitute = &c;
                    substitute_evidence = alt.first;
                    break;
                }
            }
            if (!substitute) {
                // Every unused candidate has now been checked and none
                // supports this text. Telling the model to "pick a different
                // candidate" here would be asking for something just proved
                // impossible — which is exactly what it used to do, and why
                // it resubmitted identical arguments until the loop detector
                // fired. Drop the item instead and keep the issue.
                const std::string reason = where + problem;
                std::cerr << "[publish_issue] dropped " << reason << "\n";
                drops.push_back(reason);
                used.erase(effective_id);
                continue;
            }
            std::cerr << "[publish_issue] item " << n << ": candidate "
                      << effective_id << " didn't support the text; "
                      << "substituted candidate " << substitute->value("id", 0)
                      << " instead\n";
            used.erase(effective_id);
            used.emplace(substitute->value("id", 0), n);
            candidate = substitute;
            evidence = substitute_evidence;
        }

        items.push_back({
            {"n",            n},
            {"emoji",        trim(sel.emoji)},
            {"text",         trim(post_text)},
            {"url",          funes::json_string(*candidate, "url")},
            {"headline",     headline_from_title(funes::json_string(*candidate, "title"))},
            {"source",       funes::json_string(*candidate, "source")},
            {"published",    funes::json_string(*candidate, "published")},
            {"evidence",     evidence},
            {"candidate_id", candidate->value("id", 0)}
        });
    }

    if (!errors.empty()) {
        out = nlohmann::json();
        std::string combined;
        for (size_t i = 0; i < errors.size(); ++i) {
            if (i > 0) combined += "\n";
            combined += errors[i];
        }
        return combined;
    }

    // Dropping is bounded. Below the publication's floor there is no issue
    // worth sending, and the reasons have to travel with the rejection — the
    // model cannot fix what it is not told.
    if (static_cast<int>(items.size()) < std::max(1, min_items)) {
        out = nlohmann::json();
        std::string combined = "only " + std::to_string(items.size()) +
            " item(s) could be grounded in the pool; this publication needs at "
            "least " + std::to_string(std::max(1, min_items)) + ".";
        for (const auto& d : drops) combined += "\n" + d;
        combined += "\nRewrite these posts so they say what the candidate's page "
                    "actually says, or choose different stories from the pool.";
        return combined;
    }

    if (dropped) *dropped = drops;

    // Renumber: a gap left by a drop would misalign post_tweet.py, which
    // addresses items by their 1-based position.
    for (size_t i = 0; i < items.size(); ++i)
        items[i]["n"] = static_cast<int>(i) + 1;

    out = {
        {"publication", funes::json_string(pool, "publication")},
        {"date",        funes::json_string(pool, "date")},
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
    // under today's headline. It used to do exactly that whenever harvest had
    // failed — see resolve_pool_date in issue.h.
    std::vector<std::string> available;
    {
        std::error_code ec;
        const std::string prefix = "harvest_" + publication + "_";
        for (const auto& e : std::filesystem::directory_iterator(workspace, ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind(prefix, 0) == 0 && e.path().extension() == ".json")
                available.push_back(name.substr(prefix.size(), 10));
        }
    }
    const PoolChoice choice =
        resolve_pool_date(available, funes::json_string(args, "date"), today_iso());
    if (!choice.error.empty()) {
        std::cerr << "[publish_issue] refused: " << choice.error << "\n";
        return {"Not published — " + choice.error, true};
    }
    const std::string date = choice.date;

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
            return {"Every entry in 'items' must be an object with id, emoji, and text.", true};
        Selection sel;
        if (!item.contains("id") || !item["id"].is_number_integer())
            return {"Every item needs an integer 'id' from the candidate pool.", true};
        sel.candidate_id = item["id"].get<int>();
        sel.emoji        = funes::json_string(item, "emoji");
        sel.text         = funes::json_string(item, "text");
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
    std::vector<std::string> dropped;
    // One chance to shorten its own prose, then the tool does it. See
    // build_issue's `trim_over_length` in issue.h for why the second attempt
    // is not left to the model. Keyed per publication+date because that is the
    // unit being published; in-process because the alternative is filesystem
    // state for something whose worst failure mode is granting one extra
    // attempt after a restart.
    const std::string attempt_key = publication + "|" + date;
    bool trim_over_length = false;
    {
        std::lock_guard<std::mutex> lock(length_retry_mu());
        trim_over_length = length_retries().count(attempt_key) > 0;
    }

    const std::string problem = build_issue(pool, selection, issue, min_items, &dropped,
                                            trim_over_length);
    if (!problem.empty()) {
        std::cerr << "[publish_issue] rejected: " << problem << "\n";
        // Only a length rejection arms the trim: any other reason is one the
        // model can actually act on, and arming it there would let a single
        // unrelated rejection silently license editing the copy later.
        if (problem.find("the limit is " + std::to_string(kMaxPostChars)) != std::string::npos) {
            std::lock_guard<std::mutex> lock(length_retry_mu());
            length_retries().insert(attempt_key);
            std::cerr << "[publish_issue] a further over-length post for " << attempt_key
                      << " will be trimmed rather than rejected\n";
        }
        write_blocked_record(run_path, publication, date, problem);
        return {"Not published — " + problem + "\nNothing was sent.", true};
    }

    // publish_issue.py's own link re-check can drop an item whose link has
    // gone bad since harvest (swapping in a same-story pool alternate, or
    // dropping it outright) — it needs this floor to know whether the
    // survivors still make a publishable issue, the same way this handler
    // just did above.
    issue["min_items"] = min_items;

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

    // Say what was lost as well as what shipped. A model that asked for ten
    // and got nine has to learn that from the tool result, not from the
    // subscriber who notices.
    std::string summary = "Published " + std::to_string(issue["items"].size()) +
                          " items for " + publication + " " + date + ".";
    if (!dropped.empty()) {
        summary += "\n" + std::to_string(dropped.size()) +
                   " item(s) were dropped because no page in the pool supported "
                   "the post text:";
        for (const auto& d : dropped) summary += "\n  " + d;
    }
    return {summary + "\n" + run.output};
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
        "Give one entry per item: the candidate's `id`, an emoji, and the post text. "
        "You never supply a URL — each id is resolved against the pool, so the link that "
        "ships is the one that was fetched and checked. "
        "Each post is automatically checked against its candidate's page text; if the post "
        "doesn't match the page (too few content words in common), the item is rejected and "
        "you are told to pick a different candidate. "
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
                                                      "written for X/LinkedIn"}}}
                        }},
                        {"required", json::array({"id", "emoji", "text"})}
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
