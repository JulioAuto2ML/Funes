// =============================================================================
// src/core/tools/fs_guard.cpp — confine file paths to a workspace directory
// =============================================================================

#include "fs_guard.h"

namespace fs = std::filesystem;

namespace funes::fsguard {

std::optional<fs::path> resolve(const fs::path& workspace_dir, const std::string& user_path) {
    if (user_path.empty()) return std::nullopt;

    std::error_code ec;
    fs::path ws = fs::weakly_canonical(workspace_dir, ec);
    if (ec) return std::nullopt;

    const fs::path given(user_path);
    fs::path candidate = given.is_absolute() ? given : workspace_dir / given;

    fs::path resolved = fs::weakly_canonical(candidate, ec);
    if (ec) return std::nullopt;

    // lexically_relative(resolved, ws) starts with ".." exactly when `resolved`
    // is not ws itself or a descendant of it — real symlink/`..` escapes are
    // caught here because weakly_canonical already resolved them above.
    const fs::path rel = resolved.lexically_relative(ws);
    if (rel.empty()) return std::nullopt;
    const std::string first = rel.begin()->string();
    if (first == "..") return std::nullopt;

    return resolved;
}

} // namespace funes::fsguard
