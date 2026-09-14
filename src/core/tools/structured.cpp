// =============================================================================
// src/core/tools/structured.cpp — write_structured / read_structured
// =============================================================================
// The pipeline counterpart of `publish_issue`: the model supplies the
// judgement (which topic, what the article says) and the tool supplies
// everything with a checkable right answer — the directory, the filename, the
// date, and whether the content is the shape the next stage will read.
//
// Three things are deliberate:
//
//   * **The model never types a path.** It names a pipeline, a stage and a
//     slug; `stage_relpath` derives the rest. A slug is sanitized rather than
//     validated, so a slug that drifts by punctuation or case is the *same*
//     entry instead of a second one nobody reads.
//   * **A schema violation writes nothing.** A malformed file that exists is
//     worse than no file: the next stage parses it and believes it. The error
//     is recoverable — it names the violation and restates the shape, so the
//     model can fix it on the next step rather than losing the run.
//   * **Reading an empty stage is not an error.** "Has this been debated
//     before?" is a normal first question, and an error answer costs a tool
//     budget slot and provokes a nudge for something that went fine.
//
// Everything lands under the caller's own workspace via fs_guard, the same
// resolver read_file/write_file/execute_shell use — see fs_guard.h for why
// there is exactly one of those.

#include "../answer_schema.h"
#include "../pipeline.h"
#include "../tools.h"
#include "fs_guard.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_READ_BYTES  = 64 * 1024;
constexpr size_t MAX_WRITE_BYTES = 256 * 1024;
constexpr int    MAX_LISTED      = 40;

std::string arg_str(const json& args, const char* key) {
    if (!args.contains(key)) return {};
    const json& v = args[key];
    if (v.is_string()) return v.get<std::string>();
    return {};
}

// The pipeline, or an error naming the ones that exist. A model that guesses
// "voc-pipeline" for "voc" should be able to correct itself from the refusal.
std::string load_pipeline(const std::string& dir, const std::string& id,
                          funes::Pipeline& out) {
    if (id.empty()) return "Missing 'pipeline' argument.";
    if (!funes::Pipeline::exists(dir, id)) {
        const auto ids = funes::Pipeline::list(dir);
        std::string known;
        for (const auto& i : ids) known += (known.empty() ? "" : ", ") + i;
        return "No pipeline named '" + id + "'. Configured pipelines: " +
               (known.empty() ? "(none)" : known) + ".";
    }
    return funes::Pipeline::load(dir, id, out);
}

std::string stage_names(const funes::Pipeline& p) {
    std::string s;
    for (const auto& stage : p.stages) s += (s.empty() ? "" : ", ") + stage.name;
    return s;
}

// The stage's contract as the model needs to see it: where its entries go and,
// for a JSON stage, the shape they must have.
std::string stage_contract(const funes::Pipeline& p, const funes::PipelineStage& s) {
    std::string out = "Stage '" + s.name + "' of pipeline '" + p.id + "'";
    if (!s.description.empty()) out += " — " + s.description;
    out += "\nEntries: " + s.dir + "/" + s.filename + " (" + s.format + ")";
    if (s.format == "json" && !s.schema.is_null() && !s.schema.empty())
        out += "\nRequired shape:\n" + s.schema.dump(2);
    return out;
}

fs::path pipeline_root(const fs::path& workspace_root, const funes::Pipeline& p,
                       const ToolContext& ctx) {
    // The pipeline's own workspace, not the calling agent's: every stage of
    // one pipeline writes to one place regardless of which agent ran it.
    return funes::fsguard::workspace_for(workspace_root, ctx.user_id, p.workspace);
}

// Entries in a stage, newest filename last (dates sort lexically, which is the
// entire reason the templates put {date} first).
std::vector<fs::path> stage_entries(const fs::path& root, const funes::PipelineStage& s) {
    std::vector<fs::path> found;
    std::error_code ec;
    const fs::path dir = root / s.dir;
    if (!fs::is_directory(dir, ec)) return found;

    // A "{slug}/draft.md" stage keeps each entry in its own directory, so the
    // entries are one level down. A recursive walk covers both layouts without
    // the config having to say which one it is.
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

// What a listing calls an entry: the path relative to the stage directory,
// which is what a later `read_structured(slug: …)` matches against.
std::string entry_label(const fs::path& root, const funes::PipelineStage& s,
                        const fs::path& p) {
    return fs::relative(p, root / s.dir).generic_string();
}

std::string listing_text(const fs::path& root, const funes::Pipeline& p,
                         const funes::PipelineStage& s) {
    const auto entries = stage_entries(root, s);
    std::string out = stage_contract(p, s) + "\n";
    if (entries.empty()) return out + "\nThis stage has no entries yet.";

    out += "\n" + std::to_string(entries.size()) + " entr" +
           (entries.size() == 1 ? "y" : "ies") + ":\n";
    int shown = 0;
    for (auto it = entries.rbegin(); it != entries.rend() && shown < MAX_LISTED;
         ++it, ++shown)
        out += "  " + entry_label(root, s, *it) + "\n";
    if (static_cast<int>(entries.size()) > shown)
        out += "  … and " + std::to_string(entries.size() - shown) + " older\n";
    return out;
}

// Entries whose path contains the slug. Substring rather than exact match
// because the filename carries a date the caller two stages downstream has no
// way to know; the newest match wins when a slug was written more than once.
std::vector<fs::path> match_slug(const fs::path& root, const funes::PipelineStage& s,
                                 const std::string& slug) {
    std::vector<fs::path> hits;
    for (const auto& p : stage_entries(root, s))
        if (entry_label(root, s, p).find(slug) != std::string::npos)
            hits.push_back(p);
    return hits;
}

ToolResult write_handler(const fs::path& workspace_root, const std::string& pipelines_dir,
                         const json& args, const ToolContext& ctx) {
    funes::Pipeline p;
    const std::string load_err = load_pipeline(pipelines_dir, arg_str(args, "pipeline"), p);
    if (!load_err.empty()) return {load_err, true};

    const std::string stage_name = arg_str(args, "stage");
    const funes::PipelineStage* stage = p.stage(stage_name);
    if (!stage)
        return {"Pipeline '" + p.id + "' has no stage '" + stage_name +
                "'. Stages, in order: " + stage_names(p) + ".", true};

    const std::string slug = funes::safe_slug(arg_str(args, "slug"));
    if (slug.empty())
        return {"Missing or unusable 'slug'. It becomes the entry's filename, so "
                "it needs at least one letter or digit — a short kebab-case "
                "phrase from the topic works ('local-ai-compliance').", true};

    if (!args.contains("content"))
        return {"Missing 'content' argument.", true};

    std::string body;
    if (stage->format == "json") {
        json content = args["content"];
        // A stringified or fenced object is the model's punctuation, not its
        // meaning — same extractor the answer schema uses on final answers.
        if (content.is_string()) {
            json parsed;
            if (!funes::extract_answer_json(content.get<std::string>(), parsed))
                return {"Stage '" + stage->name + "' takes JSON, and 'content' was "
                        "text with no JSON in it. Send the object itself.\n\n" +
                        stage_contract(p, *stage), true};
            content = std::move(parsed);
        }
        const std::string violation = funes::validate_answer(stage->schema, content);
        if (!violation.empty())
            return {"Refusing to write " + stage->dir + "/: " + violation +
                    ". Nothing was written — fix the content and call "
                    "write_structured again.\n\n" + stage_contract(p, *stage), true};
        body = content.dump(2);
    } else {
        const json& content = args["content"];
        body = content.is_string() ? content.get<std::string>() : content.dump(2);
    }

    if (body.size() > MAX_WRITE_BYTES)
        return {"Content is " + std::to_string(body.size()) + " bytes; the limit is " +
                std::to_string(MAX_WRITE_BYTES) + ".", true};

    const std::string date    = funes::safe_date(arg_str(args, "date"));
    const std::string relpath = funes::stage_relpath(*stage, date, slug);
    const fs::path root       = pipeline_root(workspace_root, p, ctx);
    auto resolved = funes::fsguard::resolve(root, relpath);
    if (!resolved)
        return {"Refusing to write outside the pipeline workspace (" + root.string() + ")",
                true};

    std::error_code ec;
    fs::create_directories(resolved->parent_path(), ec);
    std::ofstream out(*resolved, std::ios::binary | std::ios::trunc);
    if (!out) return {"Could not write " + resolved->string(), true};
    out << body;
    out.close();

    return {"Wrote " + relpath + " (" + std::to_string(body.size()) +
            " bytes) to pipeline '" + p.id + "', stage '" + stage->name +
            "'. Refer to this entry by its slug '" + slug + "'.", false};
}

ToolResult read_handler(const fs::path& workspace_root, const std::string& pipelines_dir,
                        const json& args, const ToolContext& ctx) {
    funes::Pipeline p;
    const std::string load_err = load_pipeline(pipelines_dir, arg_str(args, "pipeline"), p);
    if (!load_err.empty()) return {load_err, true};

    const std::string stage_name = arg_str(args, "stage");
    const funes::PipelineStage* stage = p.stage(stage_name);
    if (!stage)
        return {"Pipeline '" + p.id + "' has no stage '" + stage_name +
                "'. Stages, in order: " + stage_names(p) + ".", true};

    const fs::path root = pipeline_root(workspace_root, p, ctx);
    const std::string slug = funes::safe_slug(arg_str(args, "slug"));
    if (slug.empty())
        return {listing_text(root, p, *stage), false};

    const auto hits = match_slug(root, *stage, slug);
    if (hits.empty())
        return {"No entry matching '" + slug + "' in stage '" + stage->name +
                "'.\n\n" + listing_text(root, p, *stage), true};

    const fs::path& chosen = hits.back();   // newest, by the date in the name
    std::ifstream in(chosen, std::ios::binary);
    if (!in) return {"Could not read " + chosen.string(), true};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    if (content.size() > MAX_READ_BYTES) {
        content.resize(MAX_READ_BYTES);
        content += "\n… [truncated at " + std::to_string(MAX_READ_BYTES) + " bytes]";
    }

    std::string header = entry_label(root, *stage, chosen);
    if (hits.size() > 1)
        header += " (newest of " + std::to_string(hits.size()) + " matching '" + slug + "')";
    return {header + ":\n\n" + content, false};
}

// The tool descriptions name the pipelines that exist at startup, so a model
// that has never seen this installation's config can still make a correct
// first call. A pipeline added later is picked up by every *call* (the config
// is read per call); only this sentence is a snapshot, which is the same
// trade-off the agent roster makes.
std::string configured_pipelines(const std::string& dir) {
    const auto ids = funes::Pipeline::list(dir);
    if (ids.empty()) return "none configured";
    std::string s;
    for (const auto& id : ids) s += (s.empty() ? "" : ", ") + id;
    return s;
}

} // namespace

void register_structured_tools(ToolRegistry& reg, const std::string& workspace_dir,
                               const std::string& pipelines_dir) {
    const fs::path root = workspace_dir;
    const std::string known = configured_pipelines(pipelines_dir);

    reg.add({
        "write_structured",
        "Write one entry of a pipeline stage. The path is derived from the "
        "pipeline config — you give a slug, not a path — and JSON content is "
        "checked against the stage's schema before anything is written, so a "
        "rejected call leaves no file behind. Pipelines: " + known + ".",
        {{"type", "object"},
         {"properties", {
             {"pipeline", {{"type", "string"},
                           {"description", "Pipeline id, e.g. 'voc'."}}},
             {"stage",    {{"type", "string"},
                           {"description", "Stage name within the pipeline, e.g. 'topic'."}}},
             {"slug",     {{"type", "string"},
                           {"description", "Short kebab-case identifier for this entry. "
                                           "Reuse the same slug across stages to keep one "
                                           "piece of work together."}}},
             {"content",  {{"description", "The entry. An object for a json stage, "
                                           "text for a text stage."}}},
             {"date",     {{"type", "string"},
                           {"description", "YYYY-MM-DD. Omit to use today."}}}}},
         {"required", json::array({"pipeline", "stage", "slug", "content"})}},
        [root, pipelines_dir](const json& args, const ToolContext& ctx) {
            return write_handler(root, pipelines_dir, args, ctx);
        }
    });

    reg.add({
        "read_structured",
        "Read one entry of a pipeline stage by slug, or — with no slug — list "
        "what the stage contains and the shape its entries must have. Call it "
        "before writing a stage you are unsure of, and before starting work "
        "that may already exist. Pipelines: " + known + ".",
        {{"type", "object"},
         {"properties", {
             {"pipeline", {{"type", "string"}, {"description", "Pipeline id."}}},
             {"stage",    {{"type", "string"}, {"description", "Stage name."}}},
             {"slug",     {{"type", "string"},
                           {"description", "Entry slug. Omit to list the stage."}}}}},
         {"required", json::array({"pipeline", "stage"})}},
        [root, pipelines_dir](const json& args, const ToolContext& ctx) {
            return read_handler(root, pipelines_dir, args, ctx);
        }
    });
}
