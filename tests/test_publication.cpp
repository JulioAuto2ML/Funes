// =============================================================================
// tests/test_publication.cpp — a publication is a config, not a fork
// =============================================================================
// The claim this file has to support is the whole point of phase 5: a second
// publication is a YAML file, and nothing else. So the last test here loads one
// that does not exist in the repo — different queries, different caps, different
// artifacts, no email channel at all — and asserts it comes back intact, using
// exactly the code path the shipped one uses.
//
// The other tests are about the defaults, and they matter for the same reason
// in reverse: an empty config must reproduce what the AI newsletter did before
// any of this was configurable, or "no behaviour change" is not true.

#include "publication.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using funes::Publication;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static fs::path scratch() {
    const fs::path dir = fs::temp_directory_path() / "funes_test_publications";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static void write_config(const fs::path& dir, const std::string& id,
                         const std::string& body) {
    std::ofstream(dir / (id + ".yaml")) << body;
}

int test_a_bare_config_keeps_every_default() {
    const fs::path dir = scratch();
    write_config(dir, "bare", "id: bare\n");

    Publication pub;
    CHECK(Publication::load(dir.string(), "bare", pub).empty());
    CHECK(pub.id == "bare");
    CHECK(pub.title == "bare");                 // falls back to the id
    CHECK(pub.subject == "bare · {date}");
    CHECK(pub.time_range == "day");
    CHECK(pub.per_query == 10);
    CHECK(pub.max_candidates == 25);
    CHECK(pub.max_age_days == 2);
    CHECK(pub.dedup_against_last_issues == 7);
    CHECK(pub.count == 10);
    CHECK(pub.min_count == 8);
    CHECK(pub.max_per_story == 2);
    CHECK(pub.queries.empty());                 // the one thing with no default
    CHECK(pub.artifacts.empty());
    CHECK(pub.channels.empty());

    fs::remove_all(dir);
    return 0;
}

int test_missing_and_malformed_configs() {
    const fs::path dir = scratch();
    Publication pub;

    CHECK(!Publication::exists(dir.string(), "nope"));
    CHECK(!Publication::load(dir.string(), "nope", pub).empty());

    // An id that isn't one can't be turned into a path.
    CHECK(!Publication::exists(dir.string(), "../../etc/passwd"));
    CHECK(!Publication::exists(dir.string(), "   "));
    CHECK(!Publication::load(dir.string(), "!!!", pub).empty());

    write_config(dir, "broken", "id: broken\n  bad indent: [\n");
    CHECK(!Publication::load(dir.string(), "broken", pub).empty());

    write_config(dir, "notamap", "- just\n- a list\n");
    CHECK(!Publication::load(dir.string(), "notamap", pub).empty());

    fs::remove_all(dir);
    return 0;
}

int test_a_config_that_cannot_be_satisfied_is_refused() {
    // min_count above count would fail at publish time with a message about the
    // model's choices rather than about this file.
    const fs::path dir = scratch();
    write_config(dir, "greedy", "id: greedy\nselection:\n  count: 5\n  min_count: 8\n");

    Publication pub;
    const std::string why = Publication::load(dir.string(), "greedy", pub);
    CHECK(why.find("min_count") != std::string::npos);
    CHECK(why.find("greedy.yaml") != std::string::npos);

    fs::remove_all(dir);
    return 0;
}

int test_the_shipped_ai_pulse_config_loads() {
    // The one that actually goes out. If this file stops parsing, the daily
    // newsletter silently falls back to defaults that are not its own.
    Publication pub;
    const std::string why = Publication::load("publications", "ai-pulse", pub);
    if (!why.empty()) {
        std::cerr << "  (publications/ai-pulse.yaml: " << why << ")\n";
        return 1;
    }
    CHECK(pub.id == "ai-pulse");
    CHECK(pub.title == "AI Pulse");
    CHECK(pub.subject.find("{date}") != std::string::npos);
    CHECK(pub.queries.size() >= 3);
    CHECK(pub.min_count == 8);
    CHECK(pub.count == 10);
    CHECK(pub.artifacts.size() == 2);
    CHECK(pub.channels.size() == 1);
    CHECK(pub.channels[0].kind == "email");

    // The voice file is prose the curator reads, so it has to be there and be
    // worth reading.
    const std::string voice = funes::voice_text("publications", pub);
    CHECK(voice.size() > 200);
    return 0;
}

int test_a_second_publication_needs_no_code() {
    // Nothing below exists in the repo. If it round-trips, a new publication is
    // a file.
    const fs::path dir = scratch();
    fs::create_directories(dir / "voice");
    std::ofstream(dir / "voice" / "security-brief.md")
        << "Write for defenders. CVEs over vendor blogs.\n";
    write_config(dir, "security-brief", R"(
id: security-brief
title: "Security Brief"
subject: "Security Brief — {iso_date}"
voice:
  prompt_file: voice/security-brief.md
sources:
  queries:
    - "critical vulnerability disclosed"
    - "ransomware incident"
  time_range: week
  per_query: 15
  max_age_days: 7
  dedup_against_last_issues: 30
  exclude_domains: [prnewswire.com]
selection:
  count: 5
  min_count: 3
  max_per_source: 1
  max_per_story: 1
artifacts:
  - kind: newsletter_html
    path: "{workspace}/security_{iso_date}.html"
    template: security_template.html
channels: []
)");

    Publication pub;
    CHECK(Publication::load(dir.string(), "security-brief", pub).empty());
    CHECK(pub.title == "Security Brief");
    CHECK(pub.subject == "Security Brief — {iso_date}");
    CHECK(pub.queries.size() == 2);
    CHECK(pub.time_range == "week");
    CHECK(pub.per_query == 15);
    CHECK(pub.max_age_days == 7);
    CHECK(pub.dedup_against_last_issues == 30);
    CHECK(pub.exclude_domains == std::vector<std::string>{"prnewswire.com"});
    CHECK(pub.count == 5);
    CHECK(pub.min_count == 3);
    CHECK(pub.max_per_source == 1);
    CHECK(pub.max_per_story == 1);

    // One artifact, not the AI newsletter's two, and no posts file at all.
    CHECK(pub.artifacts.size() == 1);
    CHECK(pub.artifacts[0].kind == "newsletter_html");
    CHECK(pub.artifacts[0].html_template == "security_template.html");
    CHECK(pub.artifacts[0].path.find("security_{iso_date}") != std::string::npos);

    // An explicitly empty channel list is a publication that renders and does
    // not send — different from having no `channels:` key at all.
    CHECK(pub.channels.empty());

    CHECK(funes::voice_text(dir.string(), pub).find("defenders") != std::string::npos);

    fs::remove_all(dir);
    return 0;
}

int test_a_missing_voice_file_is_not_an_error() {
    const fs::path dir = scratch();
    write_config(dir, "quiet", "id: quiet\nvoice:\n  prompt_file: voice/gone.md\n");
    Publication pub;
    CHECK(Publication::load(dir.string(), "quiet", pub).empty());
    CHECK(funes::voice_text(dir.string(), pub).empty());
    fs::remove_all(dir);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_a_bare_config_keeps_every_default();
    rc |= test_missing_and_malformed_configs();
    rc |= test_a_config_that_cannot_be_satisfied_is_refused();
    rc |= test_the_shipped_ai_pulse_config_loads();
    rc |= test_a_second_publication_needs_no_code();
    rc |= test_a_missing_voice_file_is_not_an_error();
    if (rc == 0) std::cout << "test_publication: all tests passed\n";
    return rc;
}
