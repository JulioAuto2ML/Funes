// =============================================================================
// src/core/tools/fs_guard.h — confine file paths to a workspace directory
// =============================================================================
// Shared by read_file, write_file, execute_shell (cwd), and the upload
// endpoint: every filesystem-touching tool operates inside one workspace
// root so an LLM-chosen path can't reach the rest of the machine via `..`,
// an absolute path, or a symlink.

#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace funes::fsguard {

// Resolves `user_path` (relative or absolute) against `workspace_dir` and
// returns the resolved absolute path only if it stays within the workspace.
// Returns std::nullopt if it would escape (via "..", a symlink, or an
// absolute path outside the workspace) or if the workspace itself doesn't
// resolve. The target does not need to exist yet (safe for write_file).
std::optional<std::filesystem::path> resolve(const std::filesystem::path& workspace_dir,
                                              const std::string& user_path);

} // namespace funes::fsguard
