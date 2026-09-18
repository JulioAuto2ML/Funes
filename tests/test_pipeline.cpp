// =============================================================================
// tests/test_pipeline.cpp — a pipeline stage is a config, not a paragraph
// =============================================================================
// The VoC pipeline's file conventions ("write topics/<date>-<slug>.json with
// these fields") lived in five system prompts, where nothing enforced them: a
// slug could drift, a required field could be dropped, and the next stage in
// the pipeline found nothing where it looked. This is that convention moved
// into a file the runtime reads.
//
// So the assertions here are about the two things prose could not do: a path
// the *tool* derives rather than the model types, and a stage schema that
// comes back as a specific violation rather than as a malformed file on disk.

#include "pipeline.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using funes::Pipeline;
using funes::PipelineStage;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

static fs::path scratch() {
    const fs::path dir = fs::temp_directory_path() / "funes_test_pipelines";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static void write_config(const fs::path& dir, const std::string& id,
                         const std::string& body) {
    std::ofstream(dir / (id + ".yaml")) << body;
}

static const char* TWO_STAGE = R"(
id: demo
description: a two-stage pipeline
workspace: data/demo
stages:
  - name: topic
    dir: topics
    filename: "{date}-{slug}.json"
    schema:
      type: object
      required: [topic, status]
      properties:
        topic:  {type: string}
        status: {type: string, enum: [pending, done]}
  - name: article
    dir: articles
    filename: "{slug}/draft.md"
    format: text
)";

int test_loads_stages_in_order() {
    const fs::path dir = scratch();
    write_config(dir, "demo", TWO_STAGE);

    Pipeline p;
    const std::string err = Pipeline::load(dir.string(), "demo", p);
    CHECK(err.empty());
    CHECK(p.id == "demo");
    CHECK(p.workspace == "data/demo");
    CHECK(p.stages.size() == 2);

    // Order is the file's order: a pipeline is a sequence, and the listing the
    // model sees has to match the order it runs them in.
    CHECK(p.stages[0].name == "topic");
    CHECK(p.stages[1].name == "article");

    const PipelineStage* topic = p.stage("topic");
    CHECK(topic != nullptr);
    CHECK(topic->dir == "topics");
    CHECK(topic->filename == "{date}-{slug}.json");
    CHECK(topic->format == "json");                  // the default
    CHECK(topic->schema.contains("required"));

    // A text stage carries no schema — an article draft is prose, and a
    // schema that only ever says "string" is a schema nobody reads.
    const PipelineStage* article = p.stage("article");
    CHECK(article != nullptr);
    CHECK(article->format == "text");
    CHECK(article->schema.is_null() || article->schema.empty());

    CHECK(p.stage("nope") == nullptr);
    return 0;
}

int test_rejects_configs_that_would_fail_later() {
    const fs::path dir = scratch();

    // No stages at all: a pipeline with nothing to write is a typo, and
    // catching it here names the file instead of the model's arguments.
    write_config(dir, "empty", "id: empty\nworkspace: data/empty\n");
    Pipeline p;
    CHECK(!Pipeline::load(dir.string(), "empty", p).empty());

    // A stage with no name can never be addressed by a tool call.
    write_config(dir, "nameless", "id: nameless\nstages:\n  - dir: x\n");
    CHECK(!Pipeline::load(dir.string(), "nameless", p).empty());

    // Two stages with the same name: stage("dup") would silently pick one.
    write_config(dir, "dup",
                 "id: dup\nstages:\n  - name: a\n    dir: x\n  - name: a\n    dir: y\n");
    CHECK(!Pipeline::load(dir.string(), "dup", p).empty());

    // A stage directory that climbs out of the pipeline workspace. fs_guard
    // would refuse the write anyway; refusing the *config* says where the
    // mistake is rather than reporting it once per call at runtime.
    write_config(dir, "escape", "id: escape\nstages:\n  - name: a\n    dir: ../../etc\n");
    CHECK(!Pipeline::load(dir.string(), "escape", p).empty());

    // A filename template with neither placeholder collapses every entry in
    // the stage onto one path — the bug is silent, so it is rejected.
    write_config(dir, "fixed",
                 "id: fixed\nstages:\n  - name: a\n    dir: x\n    filename: out.json\n");
    CHECK(!Pipeline::load(dir.string(), "fixed", p).empty());

    // Missing file.
    CHECK(!Pipeline::load(dir.string(), "absent", p).empty());
    CHECK(!Pipeline::exists(dir.string(), "absent"));
    return 0;
}

int test_paths_are_derived_not_typed() {
    PipelineStage s;
    s.dir      = "topics";
    s.filename = "{date}-{slug}.json";

    CHECK(funes::stage_relpath(s, "2026-09-14", "local-ai-compliance") ==
          "topics/2026-09-14-local-ai-compliance.json");

    // A model-chosen slug is a filename component, so it is sanitized rather
    // than trusted: spaces and punctuation collapse, case flattens, and the
    // separators that would make it a different path are simply not in the
    // alphabet that survives.
    CHECK(funes::safe_slug("Local AI Compliance!") == "local-ai-compliance");
    CHECK(funes::safe_slug("  spaced   out  ") == "spaced-out");
    CHECK(funes::safe_slug("../../etc/passwd") == "etc-passwd");
    CHECK(funes::safe_slug("Ünïcödé") == "ncd");            // ASCII-only: non-ASCII bytes drop out
    CHECK(funes::safe_slug("").empty());
    CHECK(funes::safe_slug("---").empty());
    CHECK(funes::safe_slug(std::string(200, 'a')).size() == 64);

    // A stage whose template has no {date} ignores the date entirely.
    PipelineStage plain;
    plain.dir      = "articles";
    plain.filename = "{slug}/draft.md";
    CHECK(funes::stage_relpath(plain, "2026-09-14", "x") == "articles/x/draft.md");

    // A date that isn't a date can't reach the filesystem through the path.
    CHECK(funes::safe_date("2026-09-14") == "2026-09-14");
    CHECK(funes::safe_date("../../etc").empty());
    CHECK(funes::safe_date("14/09/2026").empty());
    return 0;
}

int test_the_shipped_voc_pipeline_loads() {
    // Runs from the project root (see tests/CMakeLists.txt): this is the
    // config the five VoC agents actually run on, so a typo in it fails the
    // suite rather than the next pipeline run.
    Pipeline p;
    const std::string err = Pipeline::load("pipelines", "voc", p);
    if (!err.empty()) std::cerr << "  voc.yaml: " << err << "\n";
    CHECK(err.empty());
    CHECK(p.stage("topic") != nullptr);
    CHECK(p.stage("debate") != nullptr);
    CHECK(p.stage("article") != nullptr);
    CHECK(p.stage("mvp") != nullptr);

    // The names in the agents' prompts and the names in the config are the
    // same names; `list` is what the tool's error message offers when a model
    // asks for a stage that isn't there.
    const auto names = Pipeline::list("pipelines");
    CHECK(std::find(names.begin(), names.end(), "voc") != names.end());
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_loads_stages_in_order();
    rc |= test_rejects_configs_that_would_fail_later();
    rc |= test_paths_are_derived_not_typed();
    rc |= test_the_shipped_voc_pipeline_loads();
    if (rc == 0) std::cout << "test_pipeline: all passed\n";
    return rc;
}
