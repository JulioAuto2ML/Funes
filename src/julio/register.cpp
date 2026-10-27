// =============================================================================
// src/julio/register.cpp — how the newsletter/VoC tools plug into the binary
// =============================================================================
// Compiled into `funes` by CMakeLists.txt; the static initializer below
// queues register() and main() runs it after the built-in tools exist (see
// core/extension.h). Everything these tools need to know about their own
// layout is resolved here, from the environment first and the checkout second.

#include "extension.h"
#include "funes_config.h"
#include "harvest.h"
#include "issue.h"
#include "tools.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>

// Declared in structured.cpp / rankings.cpp (they used to be in core/tools.h).
void register_structured_tools(ToolRegistry& reg, const std::string& workspace_dir,
                               const std::string& pipelines_dir);
void register_ranking_tools(ToolRegistry& reg);

namespace {

// FUNES_ROOT is baked in by CMakeLists.txt as the checkout's path, so a plain
// build finds publications/, pipelines/ and publishing/ without any
// environment. Each may still be pointed
// elsewhere with the same FUNES_*_DIR variables the pre-split binary read.
std::string dir_of(const char* env_var, const char* relative) {
    const std::string configured = funes::env(env_var);
    if (!configured.empty()) return std::filesystem::absolute(configured).string();
    return (std::filesystem::path(FUNES_ROOT) / relative).string();
}

// The MCP servers the gmail/whatsapp agents spawn live in this checkout, and
// their agent YAML names them as ${FUNES_ROOT}/third-party/... — a path
// that differs between a dev box and the deployment host, so it cannot be
// committed literally without one machine hand-editing it back after a pull.
// Publishing it from the path the build already baked in means neither machine
// configures anything; funes.local still wins if it sets the variable itself.
// Safe to do here: register_all_extensions() runs before FunesApi is
// constructed, and that constructor is what first parses the agent YAMLs.
void publish_checkout_root() {
    if (!funes::env("FUNES_ROOT").empty()) return;   // an override stands
    ::setenv("FUNES_ROOT", FUNES_ROOT, 0);
}

void register_julio_tools(const funes::ext::Services& s) {
    publish_checkout_root();
    const std::string publications_dir = dir_of("FUNES_PUBLICATIONS_DIR", "publications");
    const std::string publishing_dir   = dir_of("FUNES_PUBLISHING_DIR",   "publishing");
    const std::string pipelines_dir    = dir_of("FUNES_PIPELINES_DIR",    "pipelines");

    register_harvest_tool(s.tools, s.memory, s.workspace_dir, publications_dir);
    register_publish_issue_tool(s.tools, s.workspace_dir, publishing_dir, publications_dir);
    register_structured_tools(s.tools, s.workspace_dir, pipelines_dir);
    register_ranking_tools(s.tools);

    std::cerr << "[julio] tools registered (publications: " << publications_dir
              << ", pipelines: " << pipelines_dir
              << ", FUNES_ROOT: " << funes::env("FUNES_ROOT") << ")\n";
}

const bool registered = (funes::ext::register_extension(&register_julio_tools), true);

} // namespace
