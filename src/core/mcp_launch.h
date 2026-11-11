// =============================================================================
// src/core/mcp_launch.h — start an MCP stdio server the way any child starts
// =============================================================================
// Until 7.6 an MCP stdio server (WhatsApp, IMAP, RSS) was the one kind of
// child the OS-plan work had not reached: the vendored client forked and
// exec'd it directly, so it ran in the server's cgroup with the server's whole
// filesystem — memory.db, funes.local's file, every account's workspace.
//
// Now it goes through the `funes _exec` trampoline (process_runner.h) like a
// script: a cgroup of its own, with the default FUNES_CHILD_* limits (its
// leftovers — npx's node child — die with it), and the sandbox: the system
// read-only, a private temp directory, and what the server's `access:` block
// in the agent YAML adds. No workspace: these servers act for the
// installation, not for whoever is calling.
// =============================================================================

#pragma once
#include "agent_config.h"
#include "mcp_stdio_client.h"
#include <memory>
#include <string>

namespace funes {

// A stdio client for `srv`, not yet started (initialize() starts it).
// `agent_name` labels the line logged when the server stops, which carries
// what it cost.
std::unique_ptr<mcp::stdio_client> make_stdio_client(const McpServerConfig& srv,
                                                     const std::string& agent_name);

} // namespace funes
