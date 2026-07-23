// =============================================================================
// src/server/api.h — Funes HTTP API
// =============================================================================
//
// Routes (all JSON unless noted):
//   GET    /api/status              — health, model info, memory stats
//   GET    /api/agents              — available agents
//   POST   /api/agents/reload       — re-read agents/*.yaml
//   POST   /api/chat                — {agent?, session, message} → SSE stream
//   GET    /api/memories            — ?agent=&q=&limit=&offset=
//   POST   /api/memories            — {agent?, text} (manual memory from UI)
//   DELETE /api/memories/<id>
//   GET    /api/history             — ?session=&limit= (restore chat on reload)
//   POST   /api/upload               — multipart 'file' → saved into the workspace,
//                                       returns a text preview for the UI to embed
//   GET    /*                       — static web UI
//
// The chat SSE stream emits:
//   event: memories | delta | tool_call | tool_result | done | error
// =============================================================================

#pragma once
#include "agent.h"
#include "memory.h"
#include "tools.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace httplib { class Server; }

class FunesApi {
public:
    FunesApi(ToolRegistry& tools, MemoryStore& memory,
             const AgentDefaults& defaults,
             const std::string& agents_dir,
             const std::string& ui_dir,
             const std::string& default_agent,
             const std::string& workspace_dir);

    // Register all routes on the given server.
    void mount(httplib::Server& srv);

    // Loads agents/*.yaml. Returns how many agents were loaded.
    size_t load_agents();

    size_t agent_count() const { return agents_.size(); }

private:
    ToolRegistry& tools_;
    MemoryStore&  memory_;
    AgentDefaults defaults_;
    std::string   agents_dir_;
    std::string   ui_dir_;
    std::string   default_agent_;
    std::string   workspace_dir_;

    std::map<std::string, AgentConfig> agents_;
    mutable std::mutex agents_mu_;

    // Returns nullptr-equivalent (empty name) if not found.
    AgentConfig find_agent(const std::string& name) const;
};
