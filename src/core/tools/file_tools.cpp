// =============================================================================
// src/core/tools/file_tools.cpp — read_file / write_file native tools
// =============================================================================
// Both are confined to one workspace directory (see fs_guard.h) — the model
// can only ever touch files under there, never the rest of the filesystem.

#include "../base64.h"
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
constexpr size_t MAX_IMAGE_BYTES       = 6 * 1024 * 1024;  // matches the ~6 MB raw budget api.cpp allows per image
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

// Where this call's files live: the caller's own directory under the
// server-wide root, with the agent's `workspace_dir` (agents/*.yaml) applied
// inside it. All the reasoning is in fs_guard.h — kept there so read_file,
// write_file, execute_shell and /api/upload cannot disagree about where a
// given user's workspace is.
fs::path effective_workspace(const fs::path& root, const ToolContext& ctx) {
    return funes::fsguard::workspace_for(root, ctx.user_id, ctx.workspace_dir);
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

    const std::string image_mime = funes::detect_image_mime(content);
    if (!image_mime.empty()) {
        if (content.size() > MAX_IMAGE_BYTES)
            return {"'" + resolved->string() + "' is too large to read as an image (" +
                    std::to_string(content.size()) + " bytes, max " +
                    std::to_string(MAX_IMAGE_BYTES) + ")", true};
        ToolResult out;
        out.text = "Attached image below — read it visually.";
        out.images.push_back({image_mime, funes::base64_encode(content)});
        return out;
    }

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

ToolResult list_files_handler(const fs::path& default_workspace, const json& args, const ToolContext& ctx) {
    const std::string dir_path = args.value("path", "");
    const fs::path workspace = effective_workspace(default_workspace, ctx);

    fs::path target = workspace;
    if (!dir_path.empty()) {
        auto resolved = funes::fsguard::resolve(workspace, dir_path);
        if (!resolved)
            return {"Refusing to list outside the workspace (" + workspace.string() + ")", true};
        target = *resolved;
    }

    std::error_code ec;
    if (!fs::is_directory(target, ec))
        return {"Not a directory: " + dir_path, true};

    json entries = json::array();
    for (const auto& entry : fs::directory_iterator(target, ec)) {
        const auto& p = entry.path();
        const std::string name = p.filename().string();
        if (name.empty() || name[0] == '.') continue;

        json item = {{"name", name}};
        if (entry.is_directory(ec)) {
            item["type"] = "directory";
        } else if (entry.is_regular_file(ec)) {
            item["type"] = "file";
            item["size"] = entry.file_size(ec);
        } else {
            continue;
        }
        entries.push_back(std::move(item));
    }

    std::sort(entries.begin(), entries.end(), [](const json& a, const json& b) {
        if (a["type"] != b["type"]) return a["type"] == "directory";
        return a["name"] < b["name"];
    });

    if (entries.empty())
        return {dir_path.empty() ? "Workspace is empty." : ("No files in " + dir_path)};

    std::string out;
    for (const auto& e : entries) {
        if (e["type"] == "directory") {
            out += e["name"].get<std::string>() + "/\n";
        } else {
            out += e["name"].get<std::string>() + "  (" +
                   std::to_string(e["size"].get<uintmax_t>()) + " bytes)\n";
        }
    }
    return {out};
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
        "extracted automatically (or rendered as images if they have no text layer). "
        "PNG/JPEG/GIF/WebP images are handed back for you to read visually (needs a "
        "vision-capable backend, max 6 MB). Other binary files are rejected. Text "
        "output capped at 64 KB.",
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

    reg.add({
        "list_files",
        "List files and subdirectories in this agent's workspace directory. "
        "Pass a relative path to list a subdirectory, or omit it to list the "
        "workspace root. Directories sort first, then files with their sizes.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description",
                    "Subdirectory to list (relative to workspace). Omit for the root."}}}
            }}
        },
        [workspace](const json& args, const ToolContext& ctx) {
            return list_files_handler(workspace, args, ctx);
        }
    });
}
