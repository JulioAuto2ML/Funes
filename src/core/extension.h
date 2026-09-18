// =============================================================================
// src/core/extension.h — native tools that live outside this repository
// =============================================================================
//
// Funes's own tools are C++ and need a rebuild to add, which is fine for the
// generic ones (memory, files, web, scripts, cron, delegation) and wrong for
// the ones that belong to one person's workflow: a newsletter harvester with
// its publication YAMLs, a Borda count for one debate pipeline, a WhatsApp
// autoresponder's identity mapping. Those used to be compiled in beside the
// generic set, and the repository ended up 19% one operator's habits.
//
// An *extension* is a directory outside this tree that the build pulls in:
//
//     cmake -B build -DFUNES_EXTENSIONS=/path/to/my-extension
//
// The directory provides `extension.cmake`, which adds its sources to the
// `funes` executable (target_sources) and, if it likes, its tests. Each source
// registers its tools the way generated HTTP tools already do — a file-scope
// static initializer queues a function, and main() applies the queue once the
// core services exist:
//
//     static const bool registered = (funes::ext::register_extension(
//         [](const funes::ext::Services& s) { register_my_tool(s.tools); }), true);
//
// Sources of the executable, not of funes_core, for the reason the generated
// tools are: a static library drops an object file nothing references, and
// nothing references a self-registering one.
//
// What an extension gets is exactly what a built-in gets — the registry, the
// memory store, the workspace root — and nothing more. Its own configuration
// (where its publication YAMLs live, which script renders an issue) is its own
// business: read it from the environment with funes::env(), default it
// relative to a root the extension's cmake bakes in as a compile definition.
// The core knows the extension exists only as a list of tool names in an
// agent's YAML, the same as it knows an MCP server.
// =============================================================================

#pragma once
#include <string>

class ToolRegistry;
class MemoryStore;

namespace funes::ext {

struct Services {
    ToolRegistry&      tools;
    MemoryStore&       memory;
    const std::string& workspace_dir;   // FUNES_WORKSPACE_DIR, resolved
};

using RegisterFn = void (*)(const Services&);

// Queue a registration function. Called from static initializers; the queue
// is a function-local static so initialization order across translation
// units does not matter.
void register_extension(RegisterFn fn);

// Apply every queued registration. Called once from main(), after the
// built-in tools. Returns how many ran.
size_t register_all_extensions(const Services& services);

} // namespace funes::ext
