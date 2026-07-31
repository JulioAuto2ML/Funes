// =============================================================================
// src/core/tools/harvest.cpp — harvest_candidates
// =============================================================================
// See harvest.h for why this exists.

#include "harvest.h"
#include "../memory.h"
#include "../publication.h"
#include "../result_store.h"
#include "../text_utils.h"
#include "../tools.h"
#include "page_text.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace funes::harvest {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Someone else's analytics. Conservative on purpose: a parameter that might
// carry meaning to the site (`ref`, `s`, `id`) is left alone, because dropping
// a load-bearing parameter turns a good link into a 404, which is the failure
// this whole file exists to prevent.
bool is_tracking_param(const std::string& name) {
    static const char* kExact[] = {
        "fbclid", "gclid", "msclkid", "yclid", "dclid", "igshid",
        "mc_cid", "mc_eid", "_hsenc", "_hsmi", "ref_src", "cmpid",
        "at_medium", "at_campaign", "vero_id", "wt_mc"
    };
    if (name.compare(0, 4, "utm_") == 0) return true;
    for (const char* k : kExact)
        if (name == k) return true;
    return false;
}

constexpr const char* kMonths[] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec"
};

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [](unsigned char c) { return std::isdigit(c); });
}

// Howard Hinnant's days_from_civil: days since 1970-01-01, valid for any
// proleptic Gregorian date. Used instead of mktime/timegm so the result never
// depends on the process timezone — a newsletter run at 23:00 in Buenos Aires
// must not decide an article is a day older than it is.
long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
}

bool split_iso(const std::string& iso, int& y, unsigned& m, unsigned& d) {
    if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') return false;
    try {
        y = std::stoi(iso.substr(0, 4));
        m = static_cast<unsigned>(std::stoi(iso.substr(5, 2)));
        d = static_cast<unsigned>(std::stoi(iso.substr(8, 2)));
    } catch (...) { return false; }
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

std::string two(int n) {
    return (n < 10 ? "0" : "") + std::to_string(n);
}

// Only [a-z0-9_-] survives, so a publication id chosen by a model can't walk
// out of the workspace through the filename it lands in.
std::string safe_id(const std::string& raw) {
    std::string out;
    for (char c : lower(raw)) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
            out += c;
        else if (!out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

} // namespace

// ── URL handling ─────────────────────────────────────────────────────────────

std::string strip_tracking(const std::string& url) {
    const size_t q = url.find('?');
    if (q == std::string::npos) return url;

    const size_t frag = url.find('#', q);
    const std::string query = url.substr(q + 1, frag == std::string::npos
                                                ? std::string::npos : frag - q - 1);
    const std::string tail = frag == std::string::npos ? "" : url.substr(frag);

    std::string kept;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(start, amp - start);
        const std::string name = pair.substr(0, pair.find('='));
        if (!pair.empty() && !is_tracking_param(lower(name))) {
            if (!kept.empty()) kept += '&';
            kept += pair;
        }
        if (amp == query.size()) break;
        start = amp + 1;
    }

    return url.substr(0, q) + (kept.empty() ? "" : "?" + kept) + tail;
}

std::string host_of(const std::string& url) {
    std::string rest = url;
    const size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    rest = lower(rest);
    const size_t colon = rest.find(':');
    if (colon != std::string::npos) rest = rest.substr(0, colon);
    if (rest.compare(0, 4, "www.") == 0) rest = rest.substr(4);
    return rest;
}

std::string canonical_url(const std::string& url) {
    std::string rest = strip_tracking(url);
    const size_t scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
    rest = rest.substr(0, rest.find('#'));

    const size_t path_start = rest.find_first_of("/?");
    std::string host = lower(rest.substr(0, path_start));
    if (host.compare(0, 4, "www.") == 0) host = host.substr(4);
    std::string path = path_start == std::string::npos ? "" : rest.substr(path_start);

    while (!path.empty() && path.back() == '/') path.pop_back();
    return host + path;
}

std::string title_key(const std::string& title) {
    std::string key;
    for (unsigned char c : title)
        if (std::isalnum(c)) key += static_cast<char>(std::tolower(c));
    return key;
}

// ── Story clustering ─────────────────────────────────────────────────────────

namespace {

// Words that appear in headlines about anything. Kept short: every entry is a
// word a story could in principle turn on, and the cost of dropping a real
// signal is two distinct stories being merged.
bool is_stop_word(const std::string& w) {
    static const std::set<std::string> kStop = {
        "the", "and", "but", "for", "with", "from", "that", "this", "these",
        "those", "its", "it's", "into", "onto", "over", "under", "after",
        "before", "during", "while", "about", "than", "then", "there", "here",
        "are", "was", "were", "been", "being", "has", "have", "had", "will",
        "would", "could", "should", "can", "may", "might", "not", "now", "new",
        "more", "most", "less", "least", "out", "off", "own", "all", "any",
        "some", "such", "who", "whom", "what", "when", "where", "why", "how",
        "amid", "says", "say", "said", "report", "reports", "reported",
        "you", "your", "our", "their", "his", "her", "them", "they"
    };
    return kStop.count(w) > 0;
}

// Headlines spell small numbers; the pool should not treat "three companies"
// and "3 companies" as different stories.
std::string digits_for_number_word(const std::string& w) {
    static const std::pair<const char*, const char*> kNumbers[] = {
        {"one", "1"}, {"two", "2"}, {"three", "3"}, {"four", "4"},
        {"five", "5"}, {"six", "6"}, {"seven", "7"}, {"eight", "8"},
        {"nine", "9"}, {"ten", "10"}, {"eleven", "11"}, {"twelve", "12"}
    };
    for (const auto& [word, digits] : kNumbers)
        if (w == word) return digits;
    return w;
}

// Not a linguist's stemmer — just enough that "hacked", "hacks" and "hacking"
// are one token. Over-stemming is harmless here because both sides of every
// comparison go through the same function.
std::string stem(std::string w) {
    if (w.size() > 4 && w.compare(w.size() - 3, 3, "ies") == 0)
        return w.substr(0, w.size() - 3) + "y";
    if (w.size() > 5 && w.compare(w.size() - 3, 3, "ing") == 0)
        return w.substr(0, w.size() - 3);
    if (w.size() > 4 && w.compare(w.size() - 2, 2, "ed") == 0)
        return w.substr(0, w.size() - 2);
    if (w.size() > 3 && w.back() == 's')
        return w.substr(0, w.size() - 1);
    return w;
}

} // namespace

std::vector<std::string> story_tokens(const std::string& title) {
    std::vector<std::string> tokens;
    std::string word;
    auto flush = [&] {
        if (word.size() > 2 && !is_stop_word(word))
            tokens.push_back(stem(digits_for_number_word(word)));
        else if (!word.empty() && std::isdigit(static_cast<unsigned char>(word[0])))
            tokens.push_back(word);          // "3" is signal even at one character
        word.clear();
    };
    for (unsigned char c : title) {
        if (std::isalnum(c)) word += static_cast<char>(std::tolower(c));
        else flush();
    }
    flush();

    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
    return tokens;
}

double title_similarity(const std::string& a, const std::string& b) {
    const std::vector<std::string> ta = story_tokens(a), tb = story_tokens(b);
    if (ta.empty() || tb.empty()) return 0.0;

    std::vector<std::string> shared;
    std::set_intersection(ta.begin(), ta.end(), tb.begin(), tb.end(),
                          std::back_inserter(shared));
    if (shared.size() < kMinSharedTokens) return 0.0;
    return static_cast<double>(shared.size()) /
           static_cast<double>(std::min(ta.size(), tb.size()));
}

// ── Query resolution ─────────────────────────────────────────────────────────

std::string resolve_queries(const nlohmann::json& args,
                            const std::vector<std::string>& configured,
                            const std::string& publication,
                            std::vector<std::string>& out) {
    std::vector<std::string> queries = configured;

    // An empty array means the same as omitting the key. A model reaching for
    // `[]` to say "I have none of my own" must fall back to the config, not
    // hit an error — that error is what forced every live run before this fix
    // to invent generic queries instead of using the configured ones.
    if (args.contains("queries") && !args["queries"].empty()) {
        if (!args["queries"].is_array())
            return "'queries' must be an array of strings.";
        queries.clear();
        for (const auto& q : args["queries"]) {
            if (!q.is_string() || q.get<std::string>().empty())
                return "Every entry in 'queries' must be a non-empty string.";
            queries.push_back(q.get<std::string>());
        }
    }
    if (queries.empty())
        return "No queries: publication '" + publication + "' has none configured, "
               "so pass 'queries' as an array of strings.";
    if (queries.size() > 6) queries.resize(6);   // six queries is ~60 hits; the pool is 25

    out = std::move(queries);
    return {};
}

// ── Dates ────────────────────────────────────────────────────────────────────

std::string iso_date(const std::string& raw) {
    // ISO-8601 in any of its shapes ("2026-07-30", "2026-07-30T10:00:00Z").
    if (raw.size() >= 10 && raw[4] == '-' && raw[7] == '-') {
        const std::string head = raw.substr(0, 10);
        int y; unsigned m, d;
        if (split_iso(head, y, m, d)) return head;
    }

    // RFC-1123 and friends: "Wed, 30 Jul 2026 10:00:00 GMT", "Jul 3, 2026".
    const std::string low = lower(raw);
    int month = 0;
    for (int i = 0; i < 12; ++i) {
        const size_t at = low.find(kMonths[i]);
        if (at != std::string::npos) { month = i + 1; break; }
    }
    if (!month) return {};

    std::string day, year;
    for (size_t i = 0; i < low.size();) {
        if (!std::isdigit(static_cast<unsigned char>(low[i]))) { ++i; continue; }
        size_t j = i;
        while (j < low.size() && std::isdigit(static_cast<unsigned char>(low[j]))) ++j;
        const std::string num = low.substr(i, j - i);
        // A time ("10:00:00") supplies 1-2 digit runs too, so the first one
        // wins and the rest are ignored — in every format that reaches here the
        // day precedes the clock.
        if (num.size() <= 2 && day.empty()) day = num;
        else if (num.size() == 4 && year.empty()) year = num;
        i = j;
    }
    if (day.empty() || year.empty()) return {};

    const int d = std::stoi(day);
    if (d < 1 || d > 31) return {};
    return year + "-" + two(month) + "-" + two(d);
}

int days_between(const std::string& from, const std::string& to) {
    int y1, y2; unsigned m1, m2, d1, d2;
    if (!split_iso(from, y1, m1, d1) || !split_iso(to, y2, m2, d2)) return 0;
    return static_cast<int>(days_from_civil(y2, m2, d2) - days_from_civil(y1, m1, d1));
}

// ── Shortlisting ─────────────────────────────────────────────────────────────

std::vector<Candidate> shortlist(const std::vector<tavily::Hit>& hits,
                                 const Filter& filter,
                                 std::vector<Rejected>* rejected) {
    const std::set<std::string> seen(filter.seen.begin(), filter.seen.end());
    std::set<std::string> urls_taken, titles_taken;
    std::map<std::string, int> per_source;
    std::map<int, int> per_story;
    int stories = 0;
    std::vector<Candidate> out;

    auto reject = [&](const std::string& url, const std::string& why) {
        if (rejected) rejected->push_back({url, why});
    };

    for (const auto& hit : hits) {
        if (hit.url.empty() || hit.title.empty()) continue;

        const std::string url = strip_tracking(hit.url);
        const std::string key = canonical_url(url);
        if (seen.count(key))        { reject(url, "ran in a recent issue");   continue; }
        if (urls_taken.count(key))  { reject(url, "duplicate url");           continue; }

        const std::string tkey = title_key(hit.title);
        if (!tkey.empty() && titles_taken.count(tkey)) {
            reject(url, "same story as an earlier candidate");
            continue;
        }

        const std::string published = iso_date(hit.published);
        if (!published.empty() && !filter.today.empty()) {
            const int age = days_between(published, filter.today);
            // A date in the future is a timezone artifact, not staleness.
            if (age > filter.max_age_days) {
                reject(url, "published " + published + ", older than " +
                            std::to_string(filter.max_age_days) + " days");
                continue;
            }
        }

        const std::string source = host_of(url);
        if (filter.max_per_source > 0 && per_source[source] >= filter.max_per_source) {
            reject(url, "already have " + std::to_string(filter.max_per_source) +
                        " from " + source);
            continue;
        }

        // Single-link clustering against what has already been accepted: the
        // first candidate of a story defines it, and search rank means that
        // first one is the best-ranked coverage of it.
        int story = 0;
        for (const auto& taken : out) {
            if (title_similarity(hit.title, taken.title) >= kStoryThreshold) {
                story = taken.story;
                break;
            }
        }
        if (story && filter.max_per_story > 0 &&
            per_story[story] >= filter.max_per_story) {
            reject(url, "already have " + std::to_string(filter.max_per_story) +
                        " takes on that story");
            continue;
        }
        if (!story) story = ++stories;

        urls_taken.insert(key);
        if (!tkey.empty()) titles_taken.insert(tkey);
        ++per_source[source];
        ++per_story[story];

        Candidate c;
        c.title     = hit.title;
        c.url       = url;
        c.source    = source;
        c.published = published;
        c.story     = story;
        out.push_back(std::move(c));
    }
    return out;
}

void renumber_stories(std::vector<Candidate>& candidates) {
    std::map<int, int> dense;
    for (auto& c : candidates)
        c.story = dense.emplace(c.story, static_cast<int>(dense.size()) + 1).first->second;
}

// ── Output ───────────────────────────────────────────────────────────────────

std::string excerpt(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;

    std::string head = text.substr(0, std::min(max_bytes + 4, text.size()));
    funes::truncate_utf8_safe(head, max_bytes);

    // Back up to a sentence end, or failing that a word end, but only within
    // the last stretch — a document with no punctuation must not lose half its
    // excerpt to this.
    const size_t floor = head.size() > 160 ? head.size() - 160 : 0;
    size_t cut = head.find_last_of('.');
    if (cut == std::string::npos || cut < floor) cut = head.find_last_of(' ');
    if (cut != std::string::npos && cut >= floor) head.resize(cut);

    return head + " …";
}

nlohmann::json pool_for_model(const std::string& publication,
                              const std::string& date,
                              const std::vector<Candidate>& candidates,
                              size_t excerpt_bytes) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& c : candidates) {
        items.push_back({
            {"id",        c.id},
            {"story",     c.story},
            {"title",     c.title},
            {"url",       c.url},
            {"source",    c.source},
            {"published", c.published},
            {"excerpt",   excerpt(c.text, excerpt_bytes)},
            {"result_id", c.result_id}
        });
    }
    return {{"publication", publication},
            {"date",        date},
            {"candidates",  items}};
}

nlohmann::json pool_record(const std::string& publication,
                           const std::string& date,
                           const std::vector<Candidate>& candidates) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& c : candidates) {
        items.push_back({
            {"id",        c.id},
            {"story",     c.story},
            {"title",     c.title},
            {"url",       c.url},
            {"source",    c.source},
            {"published", c.published},
            {"result_id", c.result_id},
            {"text",      c.text}
        });
    }
    return {{"publication", publication},
            {"date",        date},
            {"candidates",  items}};
}

std::vector<std::string> recently_published(const std::string& issues_dir, int issues) {
    std::vector<std::string> out;
    if (issues <= 0) return out;

    std::error_code ec;
    if (!std::filesystem::is_directory(issues_dir, ec)) return out;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(issues_dir, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json")
            files.push_back(entry.path());

    // Issue files are named by date, so filename order is chronological order.
    std::sort(files.begin(), files.end(), std::greater<>());
    if (static_cast<int>(files.size()) > issues) files.resize(issues);

    for (const auto& path : files) {
        std::ifstream in(path);
        if (!in) continue;
        nlohmann::json j;
        try { in >> j; } catch (...) { continue; }
        if (!j.contains("items") || !j["items"].is_array()) continue;
        for (const auto& item : j["items"]) {
            const std::string url = item.value("url", "");
            if (!url.empty()) out.push_back(canonical_url(url));
        }
    }
    return out;
}

// ── The tool ─────────────────────────────────────────────────────────────────

namespace {

std::string today_local() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    return std::to_string(tm.tm_year + 1900) + "-" + two(tm.tm_mon + 1) + "-" + two(tm.tm_mday);
}

// Every other default is a Publication field (publication.h), so the config
// file and the tool cannot disagree about what "default" means.
//
// kExcerptBytes was 600 until two live runs on 2026-07-31 showed a curator
// agent reliably spending most of its read_result budget re-reading one
// candidate's page across several offsets rather than moving on — the excerpt
// alone wasn't enough for it to commit to an evidence quote. Raised to 1200 so
// a candidate can usually be judged and quoted from the pool alone; per the
// same math as before, 25 candidates at ~1200 chars is ~10K tokens, still well
// under curator's 32768 context_limit.
constexpr size_t kExcerptBytes         = 1200;
constexpr size_t kMaxPageBytes         = 64 * 1024;

ToolResult harvest_handler(MemoryStore& memory, const std::string& default_workspace,
                           const std::string& publications_dir,
                           const json& args, const ToolContext& ctx) {
    const std::string publication = safe_id(args.value("publication", "ai-pulse"));
    if (publication.empty())
        return {"'publication' must contain at least one letter or digit.", true};

    // The config supplies everything the caller didn't. A publication with no
    // config still works — the defaults in publication.h are what the AI
    // newsletter ran on before any of this was configurable — but then the
    // queries have to come from the call.
    funes::Publication pub;
    const bool configured = funes::Publication::exists(publications_dir, publication);
    if (configured) {
        const std::string problem =
            funes::Publication::load(publications_dir, publication, pub);
        if (!problem.empty())
            return {"Publication '" + publication + "' is misconfigured: " + problem, true};
    }

    std::vector<std::string> queries;
    {
        const std::string problem = resolve_queries(args, pub.queries, publication, queries);
        if (!problem.empty()) return {problem, true};
    }

    std::string date = args.value("date", "");
    if (!date.empty() && iso_date(date) != date)
        return {"'date' must be YYYY-MM-DD.", true};
    if (date.empty()) date = today_local();

    const int per_query = std::clamp(args.value("per_query", pub.per_query), 1, 20);
    const int max_candidates =
        std::clamp(args.value("max_candidates", pub.max_candidates), 1, 40);
    const int max_age_days = std::clamp(args.value("max_age_days", pub.max_age_days), 0, 30);
    const int max_per_source = std::max(0, args.value("max_per_source", pub.max_per_source));
    const int max_per_story  = std::max(0, args.value("max_per_story", pub.max_per_story));
    const int dedup_issues = std::max(0, args.value("dedup_against_last_issues",
                                                    pub.dedup_against_last_issues));
    const std::string time_range = args.value("time_range", pub.time_range);

    // ── 1. search ────────────────────────────────────────────────────────────
    std::vector<tavily::Hit> hits;
    std::vector<std::string> query_errors;
    for (const auto& q : queries) {
        tavily::Query tq;
        tq.query       = q;
        tq.topic       = "news";
        tq.time_range  = time_range;
        tq.max_results = per_query;
        if (!pub.exclude_domains.empty()) {
            tq.exclude_domains = tavily::default_exclude_domains();
            tq.exclude_domains.insert(tq.exclude_domains.end(),
                                      pub.exclude_domains.begin(),
                                      pub.exclude_domains.end());
        }
        std::vector<tavily::Hit> got;
        const std::string err = tavily::search(tq, got);
        if (!err.empty()) query_errors.push_back(q + ": " + err);
        else hits.insert(hits.end(), got.begin(), got.end());
    }
    if (hits.empty()) {
        std::string why = "No search results for any query.";
        for (const auto& e : query_errors) why += "\n  " + e;
        return {why, true};
    }

    // ── 2-3. dedup + recency ─────────────────────────────────────────────────
    const std::filesystem::path workspace =
        ctx.workspace_dir.empty() ? default_workspace : ctx.workspace_dir;
    const std::filesystem::path issues_dir = workspace / "issues" / publication;

    Filter filter;
    filter.today          = date;
    filter.max_age_days   = max_age_days;
    filter.max_per_source = max_per_source;
    filter.max_per_story  = max_per_story;
    filter.seen           = recently_published(issues_dir.string(), dedup_issues);

    std::vector<Rejected> rejected;
    std::vector<Candidate> pool = shortlist(hits, filter, &rejected);

    // ── 4. fetch, dropping anything that doesn't come back ───────────────────
    std::vector<Candidate> kept;
    int fetch_failures = 0;
    for (auto& c : pool) {
        if (static_cast<int>(kept.size()) >= max_candidates) break;
        funes::web::Page page = funes::web::fetch_readable(c.url);
        if (!page.error.empty()) {
            // The pre-flight link check. A candidate that can't be fetched now
            // is a candidate that can't be link-checked at publish time either,
            // so it dies here rather than after a model has written about it.
            ++fetch_failures;
            rejected.push_back({c.url, "fetch failed: " + page.error});
            continue;
        }
        if (page.text.size() > kMaxPageBytes)
            funes::truncate_utf8_safe(page.text, kMaxPageBytes);
        c.text = std::move(page.text);
        kept.push_back(std::move(c));
    }

    if (kept.empty())
        return {"Harvested nothing: " + std::to_string(hits.size()) + " search hit(s), "
                "all dropped (" + std::to_string(fetch_failures) + " failed to fetch). "
                "Try a wider time_range or different queries.", true};

    // ── 5. ids + full text in the result store ───────────────────────────────
    renumber_stories(kept);
    for (size_t i = 0; i < kept.size(); ++i) {
        kept[i].id = static_cast<int>(i) + 1;
        try {
            kept[i].result_id = memory.store_result(ctx.session, ctx.agent,
                                                    "harvest_candidates", kept[i].text);
        } catch (const std::exception& e) {
            // The excerpt is still in the pool, so the model can work without
            // the handle; only the "read more before deciding" path is lost.
            std::cerr << "[harvest] could not store page text for " << kept[i].url
                      << ": " << e.what() << "\n";
        }
    }

    // The pool file is what the publisher resolves ids against. Written before
    // the tool returns, so a model that answers immediately still has a pool
    // behind it.
    const std::filesystem::path pool_path =
        workspace / ("harvest_" + publication + "_" + date + ".json");
    std::error_code ec;
    std::filesystem::create_directories(workspace, ec);
    {
        std::ofstream out(pool_path);
        if (!out)
            return {"Harvested " + std::to_string(kept.size()) + " candidates but could not "
                    "write the pool file at " + pool_path.string() + " — publishing would "
                    "have nothing to resolve ids against.", true};
        out << funes::dump_safe(pool_record(publication, date, kept));
    }

    json result = pool_for_model(publication, date, kept, kExcerptBytes);
    result["harvested"] = kept.size();
    result["dropped"]   = rejected.size();
    result["pool_file"] = pool_path.filename().string();
    if (configured) {
        result["title"] = pub.title;
        result["select"] = {{"count", pub.count}, {"min_count", pub.min_count}};
        // The voice rides back with the pool so one generic curator can produce
        // any publication: the tool knows which one this is, the agent doesn't
        // have to.
        const std::string voice = funes::voice_text(publications_dir, pub);
        if (!voice.empty()) result["voice"] = voice;
    }
    return {funes::dump_safe(result)};
}

} // namespace
} // namespace funes::harvest

void register_harvest_tool(ToolRegistry& reg, MemoryStore& memory,
                           const std::string& workspace_dir,
                           const std::string& publications_dir) {
    reg.add({
        funes::kPoolTool,
        "Build the day's candidate pool for a publication: runs the given news searches, "
        "removes duplicates and anything that already ran in a recent issue, drops stale "
        "items, fetches every survivor (anything that fails to fetch is dropped here, so "
        "every candidate you are offered is one whose link resolves), and returns them "
        "numbered with an excerpt each. "
        "Pick items from this pool by their `id` — you never need to copy a URL, and "
        "publishing resolves ids against the pool rather than against anything you type. "
        "Use `read_result` with a candidate's `result_id` to read more of its page before "
        "deciding.",
        {
            {"type", "object"},
            {"properties", {
                {"queries", {{"type", "array"}, {"items", {{"type", "string"}}},
                             {"description", "Search queries to run (up to 6). Omit to use "
                                             "the publication's configured queries, which is "
                                             "usually right; pass your own to widen a thin "
                                             "pool."}}},
                {"publication", {{"type", "string"},
                                 {"description", "Publication id, e.g. 'ai-pulse' (default). "
                                                 "Supplies the queries, the recency window and "
                                                 "the caps, and scopes the 'already ran' check."}}},
                {"date", {{"type", "string"},
                          {"description", "Issue date YYYY-MM-DD (default: today). You do not "
                                          "need to look this up."}}},
                {"time_range", {{"type", "string"},
                                {"enum", json::array({"day", "week", "month", "year"})},
                                {"description", "Search recency window (default 'day')"}}},
                {"per_query", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20},
                               {"description", "Results per query (default 10)"}}},
                {"max_candidates", {{"type", "integer"}, {"minimum", 1}, {"maximum", 40},
                                    {"description", "Pool size cap (default 25)"}}},
                {"max_age_days", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30},
                                  {"description", "Drop items published more than this many "
                                                  "days before the issue date (default 2)"}}},
                {"max_per_source", {{"type", "integer"}, {"minimum", 0},
                                    {"description", "Cap candidates from any one site "
                                                    "(default 0 = no cap)"}}},
                {"max_per_story", {{"type", "integer"}, {"minimum", 0},
                                   {"description", "Cap candidates covering the same story "
                                                   "(default 2 = you still get a choice of "
                                                   "outlet; 0 = no cap)"}}},
                {"dedup_against_last_issues", {{"type", "integer"}, {"minimum", 0},
                                               {"description", "Drop anything that ran in this "
                                                               "many recent issues (default 7)"}}}
            }},
            // Nothing is required: `publication` defaults to ai-pulse and
            // `queries` should normally be omitted (see its description) so the
            // publication's own queries are used. Marking `queries` required
            // here — fixed 2026-07-31 — meant a model could never actually
            // follow that instruction: the schema told it to pass one, so on
            // every live run it either sent `[]` or invented generic queries of
            // its own instead of using the configured ones.
            {"required", json::array()}
        },
        [&memory, workspace_dir, publications_dir](const json& args, const ToolContext& ctx) {
            return funes::harvest::harvest_handler(memory, workspace_dir, publications_dir,
                                                   args, ctx);
        }
    });
}
