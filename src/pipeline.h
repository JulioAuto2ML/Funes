// =============================================================================
// src/core/pipeline.h — a multi-stage pipeline as configuration
// =============================================================================
// The newsletter lesson, applied a second time: don't ask the model for what a
// tool can supply. `publications/*.yaml` took the queries, caps and channels
// out of two agent prompts; this takes the *file conventions* out of five.
//
// The VoC pipeline (voc-researcher → council-chair → council-panelist ×3 →
// content-writer → mvp-builder) passes work between stages as files:
// `topics/<date>-<slug>.json`, `debates/<date>-<slug>.json`,
// `articles/<slug>/draft.md`. Until now that convention was a paragraph in
// each agent's system prompt, which means it was advice. A slug that drifted
// by one character, a required field the model dropped, an object written
// where an array was expected — all of them produce a file that exists, so
// the stage reports success and the *next* stage finds nothing where it
// looked. That failure is invisible until the end of the pipeline.
//
// A stage here is: a directory, a filename template, and (for JSON stages) the
// schema its content must match. The tools in tools/structured.cpp derive the
// path from the template — the model supplies a slug, never a path — and
// reject content that doesn't validate, with the same subset-of-JSON-Schema
// validator the agent loop already uses on final answers (answer_schema.h).
// One validator, so a stage contract and an answer contract fail the same way
// and read the same way to the small local models that have to act on them.
//
// What is deliberately *not* here: anything about what a stage means, who runs
// it, or what makes its content good. Those are judgements, and judgements
// stay in the agent. This file only knows where things go and what shape they
// are — the parts with a checkable right answer.

#pragma once
#include "json.hpp"
#include <string>
#include <vector>

namespace funes {

struct PipelineStage {
    std::string name;        // what a tool call addresses it by: "topic", "debate"
    std::string dir;         // relative to the pipeline workspace: "topics"
    // {date} and {slug} are substituted at call time. A template with neither
    // is rejected at load: every entry in the stage would land on one path.
    std::string filename = "{slug}.json";
    std::string format   = "json";   // "json" | "text"
    // JSON stages only. The answer_schema.h subset — type, required,
    // properties, items, enum, minItems/maxItems.
    nlohmann::json schema;
    // One line, shown to the model when it lists the pipeline. Prose about the
    // stage's purpose belongs in the agent prompt; this is a label.
    std::string description;
};

struct Pipeline {
    std::string id;
    std::string description;
    // Relative to the per-user workspace root, resolved through
    // fs_guard::workspace_for like any other workspace override. The pipeline
    // config owns this rather than the calling agent's `workspace_dir`, so
    // every stage of a pipeline writes to one place no matter which agent runs
    // it — which is the property the prose version kept failing to hold.
    std::string workspace = "data/{id}";
    std::vector<PipelineStage> stages;

    // Empty return = `out` is loaded. Unlike Publication::load, a missing
    // config is an error here: there is no useful default pipeline, and a
    // typo'd pipeline name must not silently become a new empty one.
    static std::string load(const std::string& dir, const std::string& id,
                            Pipeline& out);
    static bool exists(const std::string& dir, const std::string& id);
    // Ids of every *.yaml in the directory, sorted. What a tool error offers
    // when the model names a pipeline that isn't there.
    static std::vector<std::string> list(const std::string& dir);

    const PipelineStage* stage(const std::string& name) const;
};

// [a-z0-9-] only, collapsed and trimmed, capped at 64 characters. A slug comes
// from the model and ends up as a filename component, so the alphabet that
// survives is the reason `../` in a slug is not a path traversal but a word.
std::string safe_slug(const std::string& raw);

// "2026-09-14", or empty if `raw` is not exactly that shape. Callers treat
// empty as "use today" rather than passing it through.
std::string safe_date(const std::string& raw);

// "topics/2026-09-14-local-ai-compliance.json" — the stage's dir and filename
// template with {date}/{slug} substituted. Both inputs are sanitized here, so
// this is the only place a stage path is ever constructed.
std::string stage_relpath(const PipelineStage& stage, const std::string& date,
                          const std::string& slug);

// Today, UTC, as YYYY-MM-DD. The tool fills the date itself: it is a fact the
// process knows and the model guesses.
std::string today_iso();

} // namespace funes
