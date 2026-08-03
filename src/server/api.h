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
//   GET    /api/sessions            — ?limit= (conversation list: preview + last activity)
//   GET    /api/jobs                — scheduled cron jobs, read-only (managed via the
//                                       schedule_job/cancel_job/run_job_now tools — see
//                                       core/tools/cron_tool.cpp)
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
#include <vector>

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

    // Returns a default-constructed (empty name) AgentConfig if not found.
    // Public so the delegate_to_agent tool (src/core/tools/delegation.cpp)
    // can look up a target persona by name.
    AgentConfig find_agent(const std::string& name) const;

    // Every loaded agent's name, for delegation error messages.
    std::vector<std::string> agent_names() const;

    // "- name: description\n" per loaded agent other than `exclude`, sorted
    // by name. Feeds AgentDefaults::agent_roster (see agent.h) so a
    // delegating agent's system prompt always reflects agents/*.yaml as
    // currently loaded, without hardcoding names in any one agent's prompt.
    std::string agent_roster(const std::string& exclude) const;

    // Lets main() inject the AgentDefaults::agent_roster callback into the
    // AgentDefaults copy FunesApi holds internally, once `api` (the roster's
    // own data source) exists. See main.cpp.
    void set_agent_roster(std::function<std::string(const std::string&)> fn) {
        defaults_.agent_roster = std::move(fn);
    }

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
};
