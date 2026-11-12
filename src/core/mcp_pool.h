// =============================================================================
// src/core/mcp_pool.h — MCP stdio servers kept alive between agent runs
// =============================================================================
// A FunesAgent is built per request, and until 7.10 it started every stdio
// server its YAML names and stopped them again at the end: `uv run` or `npx`
// start-up (0.5–2 s) before the first token, and the vendored client's
// SIGTERM-then-wait (up to 2 s) after the last, on every WhatsApp, Gmail and
// RSS run.
//
// Now a run *leases* a started, initialised server from this pool and hands it
// back when the run ends. A lease is exclusive — the vendored client does not
// serialise writes to its pipe, and two runs interleaving JSON-RPC lines on one
// stdin would be a bug nobody could reproduce — so two runs of one agent at
// once get two servers, and the second goes back to the pool too, up to
// FUNES_MCP_POOL_IDLE_MAX per server. A server idle for FUNES_MCP_IDLE_SECONDS
// (600) is stopped. FUNES_MCP_KEEP_ALIVE=0 restores the old per-run start/stop.
//
// What is shared is safe to share because it already was: a stdio server acts
// for the installation (mcp_launch.h — no workspace, the agent YAML's env and
// `access:`), so the next caller gets the same server the last one did, never
// a different one. The key is the agent plus everything that configures the
// server, so an edited YAML gets a fresh process instead of a stale one.
//
// A lease is dropped rather than returned when a call through it threw (the
// server timed out, died, or wrote garbage — its state is unknown) or the
// server is no longer running. A dead server is never handed out.
// =============================================================================

#pragma once
#include "agent_config.h"
#include "mcp_stdio_client.h"
#include <memory>
#include <string>
#include <vector>

namespace funes {

class McpPool;

class McpLease {
public:
    McpLease() = default;
    McpLease(McpLease&&) noexcept;
    McpLease& operator=(McpLease&&) noexcept;
    McpLease(const McpLease&) = delete;
    McpLease& operator=(const McpLease&) = delete;
    ~McpLease();   // returns the server to the pool, or stops it

    explicit operator bool() const { return client_ != nullptr; }
    mcp::stdio_client* client() const { return client_.get(); }
    const std::vector<mcp::tool>& tools() const { return tools_; }
    bool reused() const { return uses_ > 1; }

    // Stop the server when the lease ends instead of returning it.
    void discard() { discard_ = true; }

private:
    friend class McpPool;
    McpPool* pool_ = nullptr;
    std::string key_;
    std::string label_;
    std::unique_ptr<mcp::stdio_client> client_;
    std::vector<mcp::tool> tools_;
    int uses_ = 0;
    bool discard_ = false;
};

struct McpPoolStats {
    size_t idle = 0;        // servers waiting in the pool
    size_t started = 0;     // ever, since the last clear()
    size_t reused = 0;      // leases served by an already-running server
};

class McpPool {
public:
    // The pool runs() uses. Settings are read from config on first use.
    static McpPool& instance();

    McpPool(bool keep_alive, int idle_seconds, size_t idle_max);
    ~McpPool();

    // A started, initialised server for `srv` as configured in `agent`, or an
    // empty lease with `error` set. `client_name` goes in MCP's initialize.
    McpLease acquire(const McpServerConfig& srv, const std::string& agent,
                     const std::string& client_name, std::string& error);

    // Stop every idle server (agents reloaded; shutdown). Leased ones are
    // stopped when their lease ends, not returned.
    void clear();
    // Stop the servers idle past the limit. A thread calls it every few
    // seconds; tests call it directly.
    void reap();
    McpPoolStats stats() const;

    // Exposed for tests.
    static std::string key_for(const McpServerConfig& srv, const std::string& agent);

private:
    friend class McpLease;
    void release(McpLease& lease);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace funes
