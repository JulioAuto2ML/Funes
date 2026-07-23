// =============================================================================
// src/core/tools/file_tools.cpp — read_file / write_file native tools
// =============================================================================
// Both are confined to one workspace directory (see fs_guard.h) — the model
// can only ever touch files under there, never the rest of the filesystem.

#include "../text_utils.h"
#include "../tools.h"
#include "fs_guard.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_READ_BYTES  = 64 * 1024;
constexpr size_t MAX_WRITE_BYTES = 256 * 1024;

ToolResult read_file_handler(const fs::path& workspace, const json& args, const ToolContext&) {
    if (!args.contains("path") || !args["path"].is_string())
        return {"Missing 'path' argument", true};

    auto resolved = funes::fsguard::resolve(workspace, args["path"].get<std::string>());
    if (!resolved)
        return {"Refusing to read outside the workspace (" + workspace.string() + ")", true};

    std::error_code ec;
    if (!fs::is_regular_file(*resolved, ec))
        return {"Not a file: " + resolved->string(), true};

    std::ifstream f(*resolved, std::ios::binary);
    if (!f) return {"Could not open " + resolved->string(), true};
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    const bool was_truncated = content.size() > MAX_READ_BYTES;
    if (was_truncated) funes::truncate_utf8_safe(content, MAX_READ_BYTES);

    if (!funes::looks_like_text(content))
        return {"'" + resolved->string() + "' looks like a binary file (" +
                std::to_string(fs::file_size(*resolved, ec)) +
                " bytes) — read_file only supports text.", true};

    if (was_truncated) content += "\n\n[content truncated at 64 KB]";
    return {content};
}

ToolResult write_file_handler(const fs::path& workspace, const json& args, const ToolContext&) {
    if (!args.contains("path") || !args["path"].is_string())
        return {"Missing 'path' argument", true};
    if (!args.contains("content") || !args["content"].is_string())
        return {"Missing 'content' argument", true};

    const std::string content = args["content"].get<std::string>();
    if (content.size() > MAX_WRITE_BYTES)
        return {"Content too large (max 256 KB)", true};

    auto resolved = funes::fsguard::resolve(workspace, args["path"].get<std::string>());
    if (!resolved)
        return {"Refusing to write outside the workspace (" + workspace.string() + ")", true};

    std::error_code ec;
    fs::create_directories(resolved->parent_path(), ec);

    const bool append = args.value("append", false);
    std::ofstream f(*resolved, append ? (std::ios::binary | std::ios::app)
                                      : (std::ios::binary | std::ios::trunc));
    if (!f) return {"Could not write " + resolved->string(), true};
    f << content;
    f.close();

    return {(append ? std::string("Appended ") : std::string("Wrote ")) +
            std::to_string(content.size()) + " bytes to " + resolved->string()};
}

} // namespace

void register_file_tools(ToolRegistry& reg, const std::string& workspace_dir) {
    fs::path workspace = workspace_dir;
    std::error_code ec;
    fs::create_directories(workspace, ec);

    reg.add({
        "read_file",
        "Read a text file from the workspace directory (" + workspace.string() + "). "
        "The path is relative to the workspace and can't escape it. Binary files are "
        "rejected. Output capped at 64 KB.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "Path relative to the workspace"}}}
            }},
            {"required", json::array({"path"})}
        },
        [workspace](const json& args, const ToolContext& ctx) {
            return read_file_handler(workspace, args, ctx);
        }
    });

    reg.add({
        "write_file",
        "Write (or append to) a text file in the workspace directory (" +
        workspace.string() + "). Creates parent directories as needed. The path is "
        "relative to the workspace and can't escape it. Content capped at 256 KB.",
        {
            {"type", "object"},
            {"properties", {
                {"path",    {{"type", "string"}, {"description", "Path relative to the workspace"}}},
                {"content", {{"type", "string"}, {"description", "Text to write"}}},
                {"append",  {{"type", "boolean"}, {"description", "Append instead of overwrite (default false)"}}}
            }},
            {"required", json::array({"path", "content"})}
        },
        [workspace](const json& args, const ToolContext& ctx) {
            return write_file_handler(workspace, args, ctx);
        }
    });
}
