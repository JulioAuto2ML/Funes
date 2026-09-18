// =============================================================================
// src/register.cpp — how this extension plugs into the Funes binary
// =============================================================================
// Compiled into `funes` by extension.cmake; the static initializer below
// queues register() and main() runs it after the built-in tools exist (see
// core/extension.h). Everything this extension needs to know about its own
// layout is resolved here, from the environment first and this repository's
// checkout second — the core never learns these directories exist.

#include "extension.h"
#include "funes_config.h"
#include "harvest.h"
#include "issue.h"
#include "tools.h"
#include <filesystem>
#include <iostream>

// Declared in structured.cpp / rankings.cpp (they used to be in core/tools.h).
void register_structured_tools(ToolRegistry& reg, const std::string& workspace_dir,
                               const std::string& pipelines_dir);
void register_ranking_tools(ToolRegistry& reg);

namespace {

// FUNES_JULIO_ROOT is baked in by extension.cmake as this checkout's path, so
// a plain `cmake -DFUNES_EXTENSIONS=...` build finds publications/, pipelines/
// and publishing/ without any environment. Each may still be pointed
// elsewhere with the same FUNES_*_DIR variables the pre-split binary read.
std::string dir_of(const char* env_var, const char* relative) {
    const std::string configured = funes::env(env_var);
    if (!configured.empty()) return std::filesystem::absolute(configured).string();
    return (std::filesystem::path(FUNES_JULIO_ROOT) / relative).string();
}

void register_julio_tools(const funes::ext::Services& s) {
    const std::string publications_dir = dir_of("FUNES_PUBLICATIONS_DIR", "publications");
    const std::string publishing_dir   = dir_of("FUNES_PUBLISHING_DIR",   "publishing");
    const std::string pipelines_dir    = dir_of("FUNES_PIPELINES_DIR",    "pipelines");

    register_harvest_tool(s.tools, s.memory, s.workspace_dir, publications_dir);
    register_publish_issue_tool(s.tools, s.workspace_dir, publishing_dir, publications_dir);
    register_structured_tools(s.tools, s.workspace_dir, pipelines_dir);
    register_ranking_tools(s.tools);

    std::cerr << "[funes-julio] tools registered (publications: " << publications_dir
              << ", pipelines: " << pipelines_dir << ")\n";
}

const bool registered = (funes::ext::register_extension(&register_julio_tools), true);

} // namespace
