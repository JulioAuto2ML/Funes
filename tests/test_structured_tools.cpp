// =============================================================================
// tests/test_structured_tools.cpp — write_structured / read_structured
// =============================================================================
// These two tools exist to make a stage contract enforceable instead of
// advisory, so the assertions that matter are the refusals: content that
// doesn't match the stage schema must leave *nothing* on disk, and a path must
// never be a thing the model typed. A malformed file that exists is worse than
// no file at all — the stage after it reads it and believes it.

#include "pipeline.h"
#include "tools.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static const char* DEMO = R"(
id: demo
workspace: data/demo
stages:
  - name: topic
    description: a topic
    dir: topics
    filename: "{date}-{slug}.json"
    schema:
      type: object
      required: [topic, pain_points, status]
      properties:
        topic: {type: string}
        pain_points:
          type: array
          minItems: 2
          items: {type: string}
        status: {type: string, enum: [pending, done]}
  - name: article
    dir: articles
    filename: "{slug}/draft.md"
    format: text
)";

struct Env {
    fs::path pipelines;
    fs::path workspace;
    fs::path stage_dir;      // where demo/topic entries land
};

static Env setup() {
    Env e;
    e.pipelines = fs::temp_directory_path() / "funes_test_structured_cfg";
    e.workspace = fs::temp_directory_path() / "funes_test_structured_ws";
    fs::remove_all(e.pipelines);
    fs::remove_all(e.workspace);
    fs::create_directories(e.pipelines);
    std::ofstream(e.pipelines / "demo.yaml") << DEMO;
    // <root>/<user_id>/<pipeline workspace>/<stage dir> — the same layout
    // read_file and write_file see, because both go through fs_guard.
    e.stage_dir = e.workspace / "1" / "data" / "demo" / "topics";
    return e;
}

static json good_topic() {
    return {{"topic", "local ai compliance"},
            {"pain_points", {"no audit trail", "cloud vendors refuse BAAs"}},
            {"status", "pending"}};
}

int test_a_valid_write_lands_where_the_config_says() {
    const Env e = setup();
    ToolRegistry reg;
    register_structured_tools(reg, e.workspace.string(), e.pipelines.string());
    CHECK(reg.has("write_structured") && reg.has("read_structured"));

    ToolContext ctx{"voc-researcher", "s1"};
    auto r = reg.call("write_structured",
                      {{"pipeline", "demo"}, {"stage", "topic"},
                       {"slug", "Local AI Compliance!"}, {"date", "2026-09-14"},
                       {"content", good_topic()}}, ctx);
    CHECK(!r.error);

    // The slug the model typed is not the slug on disk: it is sanitized, so
    // "Local AI Compliance!" and "local-ai-compliance" are the same entry and
    // the next stage finds it either way.
    const fs::path written = e.stage_dir / "2026-09-14-local-ai-compliance.json";
    CHECK(fs::exists(written));
    // The tool reports the path it derived, so the model can quote it without
    // having had to construct it.
    CHECK(r.text.find("topics/2026-09-14-local-ai-compliance.json") != std::string::npos);

    std::ifstream in(written);
    json on_disk = json::parse(in, nullptr, false);
    CHECK(!on_disk.is_discarded());
    CHECK(on_disk["topic"] == "local ai compliance");
    return 0;
}

int test_schema_violations_write_nothing() {
    const Env e = setup();
    ToolRegistry reg;
    register_structured_tools(reg, e.workspace.string(), e.pipelines.string());
    ToolContext ctx{"voc-researcher", "s1"};

    auto call = [&](json content) {
        return reg.call("write_structured",
                        {{"pipeline", "demo"}, {"stage", "topic"},
                         {"slug", "x"}, {"date", "2026-09-14"},
                         {"content", std::move(content)}}, ctx);
    };

    // Missing a required field.
    json missing = good_topic();
    missing.erase("status");
    auto r1 = call(missing);
    CHECK(r1.error);
    CHECK(r1.text.find("status") != std::string::npos);

    // Right fields, wrong shape.
    json thin = good_topic();
    thin["pain_points"] = json::array({"only one"});
    auto r2 = call(thin);
    CHECK(r2.error);
    CHECK(r2.text.find("pain_points") != std::string::npos);

    // Right shape, value outside the enum.
    json bad_status = good_topic();
    bad_status["status"] = "in-progress";
    CHECK(call(bad_status).error);

    // The refusal is the whole point: nothing reached the filesystem, so the
    // next stage finds no file rather than a plausible broken one.
    CHECK(!fs::exists(e.stage_dir / "2026-09-14-x.json"));
    std::error_code ec;
    CHECK(!fs::exists(e.stage_dir, ec) || fs::is_empty(e.stage_dir, ec));

    // And the error carries the shape, so a 7B model can fix it in one turn
    // instead of guessing at what "invalid" meant.
    CHECK(r1.text.find("required") != std::string::npos);
    return 0;
}

int test_unknown_names_list_the_known_ones() {
    const Env e = setup();
    ToolRegistry reg;
    register_structured_tools(reg, e.workspace.string(), e.pipelines.string());
    ToolContext ctx{"voc-researcher", "s1"};

    auto bad_stage = reg.call("write_structured",
                              {{"pipeline", "demo"}, {"stage", "topics"},
                               {"slug", "x"}, {"content", good_topic()}}, ctx);
    CHECK(bad_stage.error);
    CHECK(bad_stage.text.find("topic") != std::string::npos);
    CHECK(bad_stage.text.find("article") != std::string::npos);

    auto bad_pipeline = reg.call("write_structured",
                                 {{"pipeline", "nope"}, {"stage", "topic"},
                                  {"slug", "x"}, {"content", good_topic()}}, ctx);
    CHECK(bad_pipeline.error);
    CHECK(bad_pipeline.text.find("demo") != std::string::npos);

    // A slug that sanitizes to nothing would otherwise produce a filename of
    // pure separators.
    CHECK(reg.call("write_structured",
                   {{"pipeline", "demo"}, {"stage", "topic"},
                    {"slug", "!!!"}, {"content", good_topic()}}, ctx).error);
    return 0;
}

int test_content_arrives_as_a_string_too() {
    const Env e = setup();
    ToolRegistry reg;
    register_structured_tools(reg, e.workspace.string(), e.pipelines.string());
    ToolContext ctx{"voc-researcher", "s1"};

    // Local models routinely stringify an object argument, and some fence it.
    // Rejecting that would be rejecting the model for its punctuation, so the
    // same extractor the answer schema uses runs here.
    auto r = reg.call("write_structured",
                      {{"pipeline", "demo"}, {"stage", "topic"}, {"slug", "s"},
                       {"date", "2026-09-14"},
                       {"content", "```json\n" + good_topic().dump() + "\n```"}}, ctx);
    CHECK(!r.error);
    CHECK(fs::exists(e.stage_dir / "2026-09-14-s.json"));

    // Prose where JSON was required is still a refusal — it has no shape to
    // check, and writing it would hand the next stage a file it can't parse.
    CHECK(reg.call("write_structured",
                   {{"pipeline", "demo"}, {"stage", "topic"}, {"slug", "p"},
                    {"content", "I decided the topic is local AI compliance."}},
                   ctx).error);
    return 0;
}

int test_text_stages_take_prose() {
    const Env e = setup();
    ToolRegistry reg;
    register_structured_tools(reg, e.workspace.string(), e.pipelines.string());
    ToolContext ctx{"content-writer", "s1"};

    auto r = reg.call("write_structured",
                      {{"pipeline", "demo"}, {"stage", "article"},
                       {"slug", "local-ai-compliance"},
                       {"content", "# Draft\n\nBody text.\n"}}, ctx);
    CHECK(!r.error);
    const fs::path draft =
        e.workspace / "1" / "data" / "demo" / "articles" / "local-ai-compliance" / "draft.md";
    CHECK(fs::exists(draft));   // nested directories created for it

    auto back = reg.call("read_structured",
                         {{"pipeline", "demo"}, {"stage", "article"},
                          {"slug", "local-ai-compliance"}}, ctx);
    CHECK(!back.error);
    CHECK(back.text.find("Body text.") != std::string::npos);
    return 0;
}

int test_read_lists_and_finds_without_a_path() {
    const Env e = setup();
    ToolRegistry reg;
    register_structured_tools(reg, e.workspace.string(), e.pipelines.string());
    ToolContext ctx{"council-chair", "s1"};

    // Empty stage: the answer is "nothing yet", not an error — checking for a
    // prior debate is a normal first step, and a failed tool call costs a
    // budget slot and a nudge.
    auto empty = reg.call("read_structured", {{"pipeline", "demo"}, {"stage", "topic"}}, ctx);
    CHECK(!empty.error);
    CHECK(empty.text.find("no entries") != std::string::npos);
    // A listing is also where the stage's contract comes from, so an agent can
    // ask what shape to write before writing it.
    CHECK(empty.text.find("pain_points") != std::string::npos);

    reg.call("write_structured",
             {{"pipeline", "demo"}, {"stage", "topic"}, {"slug", "alpha"},
              {"date", "2026-09-10"}, {"content", good_topic()}}, ctx);
    reg.call("write_structured",
             {{"pipeline", "demo"}, {"stage", "topic"}, {"slug", "beta"},
              {"date", "2026-09-14"}, {"content", good_topic()}}, ctx);

    auto listing = reg.call("read_structured", {{"pipeline", "demo"}, {"stage", "topic"}}, ctx);
    CHECK(!listing.error);
    CHECK(listing.text.find("alpha") != std::string::npos);
    CHECK(listing.text.find("beta") != std::string::npos);

    // Fetch by slug alone: the date is part of the filename, and an agent two
    // stages downstream knows the slug but not when the entry was written.
    auto one = reg.call("read_structured",
                        {{"pipeline", "demo"}, {"stage", "topic"}, {"slug", "alpha"}}, ctx);
    CHECK(!one.error);
    CHECK(one.text.find("local ai compliance") != std::string::npos);

    // A slug with no entry says what is there instead of just failing.
    auto missing = reg.call("read_structured",
                            {{"pipeline", "demo"}, {"stage", "topic"},
                             {"slug", "gamma"}}, ctx);
    CHECK(missing.error);
    CHECK(missing.text.find("alpha") != std::string::npos);
    return 0;
}

int test_entries_are_per_user() {
    const Env e = setup();
    ToolRegistry reg;
    register_structured_tools(reg, e.workspace.string(), e.pipelines.string());

    ToolContext mine{"voc-researcher", "s1", "", "", /*user_id=*/1};
    ToolContext theirs{"voc-researcher", "s2", "", "", /*user_id=*/2};

    reg.call("write_structured",
             {{"pipeline", "demo"}, {"stage", "topic"}, {"slug", "mine"},
              {"date", "2026-09-14"}, {"content", good_topic()}}, mine);

    // A pipeline workspace resolves through fs_guard::workspace_for like every
    // other workspace path, so it is per-account by construction rather than
    // by a check somebody has to remember to write.
    CHECK(fs::exists(e.workspace / "1" / "data" / "demo" / "topics" /
                     "2026-09-14-mine.json"));
    auto other = reg.call("read_structured",
                          {{"pipeline", "demo"}, {"stage", "topic"}, {"slug", "mine"}},
                          theirs);
    CHECK(other.error);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_a_valid_write_lands_where_the_config_says();
    rc |= test_schema_violations_write_nothing();
    rc |= test_unknown_names_list_the_known_ones();
    rc |= test_content_arrives_as_a_string_too();
    rc |= test_text_stages_take_prose();
    rc |= test_read_lists_and_finds_without_a_path();
    rc |= test_entries_are_per_user();
    if (rc == 0) std::cout << "test_structured_tools: all passed\n";
    return rc;
}
