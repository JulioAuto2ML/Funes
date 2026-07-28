// =============================================================================
// src/core/tools/file_tools.cpp — read_file / write_file native tools
// =============================================================================
// Both are confined to one workspace directory (see fs_guard.h) — the model
// can only ever touch files under there, never the rest of the filesystem.

#include "../text_utils.h"
#include "../tools.h"
#include "fs_guard.h"
#include "pdf_extract.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_READ_BYTES        = 64 * 1024;
constexpr size_t MAX_WRITE_BYTES       = 256 * 1024;
constexpr int    PDF_EXTRACT_TIMEOUT_S = 15;

bool has_extension(const fs::path& p, const char* ext) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ext;
}

ToolResult extract_pdf_text(const fs::path& workspace, const fs::path& resolved) {
    funes::pdf::ExtractResult r =
        funes::pdf::extract_text(resolved, workspace, PDF_EXTRACT_TIMEOUT_S, MAX_READ_BYTES);
    if (r.ok) return {r.text_or_error, false};

    // No text layer (e.g. a scanned/photographed bill wrapped in a PDF) —
    // fall back to rendering pages as images so a vision-capable model can
    // still read them, instead of dead-ending on a text-extraction error.
    funes::pdf::RenderResult rendered = funes::pdf::render_pages_as_images(
        resolved, workspace, PDF_EXTRACT_TIMEOUT_S, /*max_pages=*/2);
    if (!rendered.ok)
        return {r.text_or_error + " Rendering fallback also failed: " + rendered.error, true};

    ToolResult out;
    out.text = "'" + resolved.string() + "' has no text layer (" + r.text_or_error +
               "). Attached " + std::to_string(rendered.pages.size()) +
               " rendered page image(s) below — read them visually instead.";
    for (auto& page : rendered.pages)
        out.images.push_back({page.mime_type, page.base64_data});
    return out;
}

// An agent's own `workspace_dir` (agents/*.yaml) overrides the server-wide
// default so a skill can be scoped to one folder without widening every
// other agent's file access.
fs::path effective_workspace(const fs::path& default_workspace, const ToolContext& ctx) {
    if (ctx.workspace_dir.empty()) return default_workspace;
    fs::path ws = ctx.workspace_dir;
    std::error_code ec;
    fs::create_directories(ws, ec);
    return ws;
}

ToolResult read_file_handler(const fs::path& default_workspace, const json& args, const ToolContext& ctx) {
    if (!args.contains("path") || !args["path"].is_string())
        return {"Missing 'path' argument", true};

    const fs::path workspace = effective_workspace(default_workspace, ctx);
    auto resolved = funes::fsguard::resolve(workspace, args["path"].get<std::string>());
    if (!resolved)
        return {"Refusing to read outside the workspace (" + workspace.string() + ")", true};

    std::error_code ec;
    if (!fs::is_regular_file(*resolved, ec))
        return {"Not a file: " + resolved->string(), true};

    if (has_extension(*resolved, ".pdf"))
        return extract_pdf_text(workspace, *resolved);

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

ToolResult write_file_handler(const fs::path& default_workspace, const json& args, const ToolContext& ctx) {
    if (!args.contains("path") || !args["path"].is_string())
        return {"Missing 'path' argument", true};
    if (!args.contains("content") || !args["content"].is_string())
        return {"Missing 'content' argument", true};

    const std::string content = args["content"].get<std::string>();
    if (content.size() > MAX_WRITE_BYTES)
        return {"Content too large (max 256 KB)", true};

    const fs::path workspace = effective_workspace(default_workspace, ctx);
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
        "Read a text file from this agent's workspace directory (default: " +
        workspace.string() + "; some agents are scoped to a different folder — the "
        "error message on a failed path will show which one applies). The path is "
        "relative to that workspace and can't escape it. .pdf files have their text "
        "extracted automatically; other binary files are rejected. Output capped at 64 KB.",
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
        "Write (or append to) a text file in this agent's workspace directory (default: " +
        workspace.string() + "; some agents are scoped to a different folder — the "
        "error message on a failed path will show which one applies). Creates parent "
        "directories as needed. The path is relative to that workspace and can't escape "
        "it. Content capped at 256 KB.",
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
