// =============================================================================
// src/core/publication.h — a publication as configuration
// =============================================================================
// "The AI newsletter" used to be spread across two agent prompts, a hardcoded
// template filename, a hardcoded posts-file naming convention and a subscriber
// list, so a second publication would have meant a fork of all four. This is
// that scattered knowledge pulled into one file, and the point is what is
// *not* in it: nothing here is a judgement.
//
// The queries, the recency window, the caps, the artifacts and the channels are
// fields because they are settings. The voice — who this is for, what to skip,
// what tone — is a separate prose file, because voice is the one part a model
// has to read rather than obey, and the moment it becomes fields someone will
// try to express "sceptical but not cynical" as an enum.
//
// The agent stays generic. `curator` has no idea it is producing AI Pulse; it
// is handed a pool and a voice note by the tools and writes what it is given.
// A second publication is a YAML file and a voice file, not a second agent.

#pragma once
#include <string>
#include <vector>

namespace funes {

struct Artifact {
    std::string kind;          // "posts_txt" | "newsletter_html"
    // {workspace} and {date} are substituted at render time. A path is a
    // template rather than a directory because the posts file's name is a
    // convention two other scripts already depend on.
    std::string path;
    std::string html_template;  // newsletter_html only; empty = the repo default
};

struct Channel {
    std::string kind;             // "email"
    std::string recipients_file;  // relative to the issue directory
};

struct Publication {
    std::string id;
    std::string title;
    // May contain {date} (e.g. "Thursday, July 30, 2026") and {iso_date}.
    // Deliberately not a strftime template: one format string in one place beats
    // a second date-formatting dialect that only this file speaks.
    std::string subject;
    std::string voice_file;     // relative to the publications directory
    // Appended to every post this publication puts on a social channel. Empty
    // by default: a call to action naming one person's inbox is not something
    // a new publication should inherit.
    std::string cta;

    // sources:
    std::vector<std::string> queries;
    std::string time_range = "day";
    int per_query                 = 10;
    int max_candidates            = 25;
    int max_age_days              = 2;
    int dedup_against_last_issues = 7;
    std::vector<std::string> exclude_domains;   // added to the global list

    // selection:
    int count          = 10;
    int min_count      = 8;
    int max_per_source = 0;
    int max_per_story  = 2;

    std::vector<Artifact> artifacts;
    std::vector<Channel>  channels;

    // Empty return = `out` is loaded. A publication that doesn't exist is not
    // an error to this function — the caller decides whether a missing config
    // means "use the defaults" (harvest, given explicit queries) or "refuse"
    // (nothing yet, but it will).
    static std::string load(const std::string& dir, const std::string& id,
                            Publication& out);

    static bool exists(const std::string& dir, const std::string& id);
};

// The voice note's text, or empty when there is no voice file. A missing file
// is not an error: a publication with no stated voice gets the model's default
// one, which is a reasonable place to start a new publication from.
std::string voice_text(const std::string& dir, const Publication& pub);

} // namespace funes
