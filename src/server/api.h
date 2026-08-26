// =============================================================================
// src/server/api.h — Funes HTTP API
// =============================================================================
//
// Routes (all JSON unless noted):
//   POST   /api/login               — {username, password} → sets session cookie  [public]
//   POST   /api/logout              — revokes the token and clears the cookie
//   GET    /api/auth/status         — {needs_bootstrap, authenticated, user?}     [public]
//   POST   /api/auth/bootstrap      — {username, password, display_name?} → first admin,
//                                       refused once any user exists              [public]
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
// Every route above except the four marked [public] requires authentication.
// That is enforced twice on purpose (see api.cpp): a pre-routing gate refuses
// any unauthenticated /api/ path so a route added later is protected by
// default, and each handler additionally resolves the caller to scope what it
// returns. The gate is the security boundary; the per-handler lookup is what
// makes the answer correct.
//
// Two ways to authenticate:
//   - session cookie  — the web UI, set by /api/login
//   - service token   — X-Funes-Service-Token, for the WhatsApp autoresponder,
//                       which pairs it with X-Funes-User-Jid naming the sender
//                       (see FUNES_SERVICE_TOKEN in config/funes.conf)
//
// The chat SSE stream emits:
//   event: memories | delta | tool_call | tool_result | done | error
// =============================================================================

#pragma once
#include "agent.h"
#include "memory.h"
#include "tools.h"
#include "users.h"
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace httplib { class Server; class Request; class Response; }

class FunesApi {
public:
    FunesApi(ToolRegistry& tools, MemoryStore& memory, UserStore& users,
             const AgentDefaults& defaults,
             const std::string& agents_dir,
             const std::string& ui_dir,
             const std::string& default_agent,
             const std::string& workspace_dir,
             const std::string& service_token);

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
    UserStore&    users_;
    AgentDefaults defaults_;
    std::string   agents_dir_;
    std::string   ui_dir_;
    std::string   default_agent_;
    std::string   workspace_dir_;
    std::string   service_token_;  // empty = service authentication disabled

    std::map<std::string, AgentConfig> agents_;
    mutable std::mutex agents_mu_;

    // ── authentication ────────────────────────────────────────────────────────

    // Resolve the caller from a session cookie or a service token. Returns
    // nothing if the request carries no valid credential. Never writes to the
    // response — callers decide what an anonymous request means.
    std::optional<UserStore::User> authenticate(const httplib::Request& req);

    // authenticate(), but writes 401 and returns nothing when there is no
    // valid credential. The `if (!user) return;` guard at the top of a handler.
    std::optional<UserStore::User> require_auth(const httplib::Request& req,
                                                httplib::Response& res);

    // require_auth(), but also refuses a member with 403. For the routes whose
    // effect reaches past the caller's own account — reloading every agent
    // definition process-wide, and anything added later that reads or writes
    // another user's data. A member hitting one is not a broken client, so it
    // is 403 (authenticated, not allowed) rather than 401.
    std::optional<UserStore::User> require_admin(const httplib::Request& req,
                                                 httplib::Response& res);

    // True for the handful of paths reachable without credentials. Anything
    // else under /api/ is refused by the pre-routing gate.
    static bool is_public_path(const std::string& path);
};
