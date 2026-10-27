// =============================================================================
// src/core/extension.h — self-registering native tool packs
// =============================================================================
//
// Used by src/julio/ (newsletter harvester, publish_issue, the VoC pipeline's
// structured tools). Between 5.1 and 5.2 that code lived in a second
// repository compiled in with -DFUNES_EXTENSIONS; it is back in this tree and
// built unconditionally (CMakeLists.txt), but it still plugs in through this
// queue rather than through main(), which keeps its directories and its
// FUNES_ROOT knowledge out of the server.
//
// Each source registers its tools the way generated HTTP tools already do — a
// file-scope static initializer queues a function, and main() applies the
// queue once the core services exist:
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
