// =============================================================================
// src/core/tools/fs_guard.h — confine file paths to a workspace directory
// =============================================================================
// Shared by read_file, write_file, execute_shell (cwd), and the upload
// endpoint: every filesystem-touching tool operates inside one workspace
// root so an LLM-chosen path can't reach the rest of the machine via `..`,
// an absolute path, or a symlink.

#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace funes::fsguard {

// The directory a tool call actually operates in, given the server-wide root,
// whose call it is, and the agent's own `workspace_dir` (agents/*.yaml).
// Creates it if needed.
//
//   no override        → <root>/<user_id>
//   relative override  → <root>/<user_id>/<override>
//   absolute override  → the override, verbatim
//
// The absolute form is a deliberate escape hatch, the filesystem counterpart
// of AgentConfig::memory_scope: it opts one agent out of per-user isolation
// into a folder every account shares. Prefer a relative path, which stays
// inside the caller's own workspace.
//
// One shipped agent uses it: curator (agents/curator.yaml), pointing at the
// publication working directory outside the Funes tree. That is what keeps
// the newsletter running against one pool of harvested candidates and one set
// of issue files rather than a copy per account — the publication is the
// installation's, not any one user's. The consequence to know: every account
// that runs the curator reads and writes the *same* files, so on a
// multi-account install two people publishing the same day would overwrite
// each other's issue. Fine while the admin is the only one who runs it, which
// is the current arrangement; revisit before granting curator to a member.
//
// Lives here rather than in file_tools.cpp so read_file, write_file,
// execute_shell and the upload endpoint cannot drift apart on where a user's
// workspace is; they all resolve the same way or the confinement below is
// guarding different roots for the same request.
std::filesystem::path workspace_for(const std::filesystem::path& root,
                                    int64_t user_id,
                                    const std::string& agent_override);

// Resolves `user_path` (relative or absolute) against `workspace_dir` and
// returns the resolved absolute path only if it stays within the workspace.
// Returns std::nullopt if it would escape (via "..", a symlink, or an
// absolute path outside the workspace) or if the workspace itself doesn't
// resolve. The target does not need to exist yet (safe for write_file).
std::optional<std::filesystem::path> resolve(const std::filesystem::path& workspace_dir,
                                              const std::string& user_path);

} // namespace funes::fsguard
