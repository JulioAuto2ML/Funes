// =============================================================================
// src/server/api.cpp — Funes HTTP API implementation
// =============================================================================

#include "api.h"
#include "../core/base64.h"
#include "../core/password.h"
#include "../core/permissions.h"
#include "../core/text_utils.h"
#include "../core/tools/fs_guard.h"
#include "../core/tools/pdf_extract.h"
#include "httplib.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

// ── helpers ───────────────────────────────────────────────────────────────────

namespace {

constexpr size_t MAX_MESSAGE_BYTES      = 16 * 1024;
constexpr size_t MAX_MEMORY_BYTES       = 4 * 1024;
constexpr size_t MAX_UPLOAD_BYTES       = 5 * 1024 * 1024;  // stored on disk in full
constexpr size_t MAX_UPLOAD_PREVIEW     = 32 * 1024;        // embedded in the chat message
constexpr size_t MAX_IMAGE_BASE64_BYTES = 8 * 1024 * 1024;  // ~6 MB raw
constexpr size_t MAX_IMAGES_PER_MESSAGE = 4;
constexpr int    PDF_UPLOAD_TIMEOUT_S   = 15;

bool looks_like_pdf(const std::string& content) {
    return content.rfind("%PDF", 0) == 0;
}

bool valid_session(const std::string& s) {
    static const std::regex re(R"(^[A-Za-z0-9_-]{1,64}$)");
    return std::regex_match(s, re);
}

void json_reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    // dump_safe, not dump(): a memory/tool-result string that isn't valid
    // UTF-8 must not be able to take down the whole response.
    res.set_content(funes::dump_safe(body), "application/json");
}

void json_error(httplib::Response& res, int status, const std::string& message) {
    json_reply(res, status, {{"ok", false}, {"error", message}});
}

// Write one SSE event to the sink. Returns false if the client disconnected.
bool sse_write(httplib::DataSink& sink, const std::string& type, const json& data) {
    std::string frame = "event: " + type + "\ndata: " + funes::dump_safe(data) + "\n\n";
    return sink.write(frame.data(), frame.size());
}

// ── authentication helpers ────────────────────────────────────────────────────

constexpr const char* COOKIE_NAME    = "funes_session";
constexpr int         SESSION_TTL_DAYS = 30;

// Pull one cookie out of a Cookie header. Hand-parsed rather than regexed
// because the header is attacker-controlled and a backtracking regex over it
// is a denial-of-service waiting to happen.
std::string read_cookie(const httplib::Request& req, const std::string& name) {
    if (!req.has_header("Cookie")) return "";
    const std::string header = req.get_header_value("Cookie");

    size_t pos = 0;
    while (pos < header.size()) {
        size_t end = header.find(';', pos);
        if (end == std::string::npos) end = header.size();

        size_t start = header.find_first_not_of(" \t", pos);
        if (start != std::string::npos && start < end) {
            size_t eq = header.find('=', start);
            if (eq != std::string::npos && eq < end) {
                if (header.compare(start, eq - start, name) == 0)
                    return header.substr(eq + 1, end - eq - 1);
            }
        }
        pos = end + 1;
    }
    return "";
}

// Session cookie. HttpOnly so page scripts can't read it; SameSite=Strict so a
// cross-site form post can't ride it. Secure is opt-in via FUNES_COOKIE_SECURE
// because the default deployment is plain HTTP on a LAN — setting it
// unconditionally would make login silently fail there, which is a worse
// failure than the one it prevents on a network the traffic never leaves.
std::string session_cookie(const std::string& token, int max_age_seconds) {
    const char* secure_env = std::getenv("FUNES_COOKIE_SECURE");
    const bool secure = secure_env && std::string(secure_env) == "1";
    return std::string(COOKIE_NAME) + "=" + token +
           "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
           std::to_string(max_age_seconds) + (secure ? "; Secure" : "");
}

} // namespace

// ── construction / agents ─────────────────────────────────────────────────────

FunesApi::FunesApi(ToolRegistry& tools, MemoryStore& memory, UserStore& users,
                   const AgentDefaults& defaults,
                   const std::string& agents_dir,
                   const std::string& ui_dir,
                   const std::string& default_agent,
                   const std::string& workspace_dir,
                   const std::string& service_token)
    : tools_(tools), memory_(memory), users_(users), defaults_(defaults)
    , agents_dir_(agents_dir), ui_dir_(ui_dir), default_agent_(default_agent)
    , workspace_dir_(workspace_dir), service_token_(service_token)
{
    load_agents();
}

// ── authentication ────────────────────────────────────────────────────────────

bool FunesApi::is_public_path(const std::string& path) {
    // Deliberately an exact-match list, not a prefix test: "/api/auth/" as a
    // prefix would make any future /api/auth/* route public by accident.
    return path == "/api/login"
        || path == "/api/auth/status"
        || path == "/api/auth/bootstrap";
}

std::optional<UserStore::User> FunesApi::authenticate(const httplib::Request& req) {
    // 1. Service token — the WhatsApp autoresponder and anything else that
    //    runs without a browser. The token proves the *caller* is trusted; it
    //    does not by itself say who the request is for, so it must be paired
    //    with a jid that maps to a real user. A service token with no jid
    //    header, or with an unmapped one, authenticates nobody — that is what
    //    keeps an incoming message from an unknown number from being answered
    //    as though it came from the admin.
    if (!service_token_.empty() && req.has_header("X-Funes-Service-Token")) {
        const std::string presented = req.get_header_value("X-Funes-Service-Token");
        if (!funes::constant_time_equals(presented, service_token_)) {
            std::cerr << "[auth] service token mismatch from " << req.remote_addr << "\n";
            return std::nullopt;
        }
        const std::string jid = req.get_header_value("X-Funes-User-Jid");
        if (jid.empty()) {
            std::cerr << "[auth] service token without X-Funes-User-Jid from "
                      << req.remote_addr << "\n";
            return std::nullopt;
        }
        auto user = users_.resolve_jid(jid);
        if (!user)
            std::cerr << "[auth] service token names unmapped jid '" << jid << "'\n";
        return user;
    }

    // 2. Session cookie — the web UI.
    const std::string token = read_cookie(req, COOKIE_NAME);
    if (token.empty()) return std::nullopt;
    return users_.resolve_token(token);
}

std::optional<UserStore::User> FunesApi::require_auth(const httplib::Request& req,
                                                      httplib::Response& res) {
    auto user = authenticate(req);
    if (!user) json_error(res, 401, "Authentication required");
    return user;
}

std::optional<UserStore::User> FunesApi::require_admin(const httplib::Request& req,
                                                       httplib::Response& res) {
    auto user = require_auth(req, res);
    if (!user) return std::nullopt;
    if (!user->is_admin()) {
        std::cerr << "[auth] '" << user->username << "' (member) refused admin route "
                  << req.path << "\n";
        json_error(res, 403, "Administrator access required");
        return std::nullopt;
    }
    return user;
}

size_t FunesApi::load_agents() {
    std::map<std::string, AgentConfig> loaded;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(agents_dir_, ec)) {
        const auto path = entry.path();
        if (path.extension() != ".yaml" && path.extension() != ".yml") continue;
        try {
            AgentConfig cfg = AgentConfig::from_file(path.string());
            loaded[cfg.name] = std::move(cfg);
        } catch (const std::exception& e) {
            std::cerr << "[api] skipping invalid agent file " << path << ": "
                      << e.what() << "\n";
        }
    }
    if (ec)
        std::cerr << "[api] cannot read agents dir '" << agents_dir_ << "': "
                  << ec.message() << "\n";

    std::lock_guard<std::mutex> lock(agents_mu_);
    agents_ = std::move(loaded);
    return agents_.size();
}

AgentConfig FunesApi::find_agent(const std::string& name) const {
    std::lock_guard<std::mutex> lock(agents_mu_);
    auto it = agents_.find(name.empty() ? default_agent_ : name);
    if (it == agents_.end()) return {};
    return it->second;
}

std::vector<std::string> FunesApi::agent_names() const {
    std::lock_guard<std::mutex> lock(agents_mu_);
    std::vector<std::string> names;
    names.reserve(agents_.size());
    for (const auto& [name, cfg] : agents_) names.push_back(name);
    return names;
}

std::string FunesApi::agent_roster(const std::string& exclude) const {
    std::lock_guard<std::mutex> lock(agents_mu_);
    std::ostringstream oss;
    for (const auto& [name, cfg] : agents_) {  // agents_ is a std::map: sorted by name
        if (name == exclude) continue;
        oss << "- " << name << ": " << cfg.description << "\n";
        if (!cfg.delegation_notes.empty())
            oss << "  Note: " << cfg.delegation_notes << "\n";
    }
    return oss.str();
}

// ── routes ────────────────────────────────────────────────────────────────────

void FunesApi::mount(httplib::Server& srv) {

    // ── auth gate ─────────────────────────────────────────────────────────────
    // Fail closed for everything under /api/ that isn't explicitly public. The
    // individual handlers authenticate again to learn *who* is calling; this
    // gate exists so that forgetting to do so in a route added later is a
    // 401 rather than a silent hole. Non-/api paths (the static UI) pass
    // through — the login page has to be reachable to log in.
    srv.set_pre_routing_handler([this](const httplib::Request& req,
                                       httplib::Response& res) {
        if (req.path.rfind("/api/", 0) != 0)
            return httplib::Server::HandlerResponse::Unhandled;
        if (is_public_path(req.path))
            return httplib::Server::HandlerResponse::Unhandled;
        if (authenticate(req))
            return httplib::Server::HandlerResponse::Unhandled;

        json_error(res, 401, "Authentication required");
        return httplib::Server::HandlerResponse::Handled;
    });

    // ── auth: bootstrap / login / logout / whoami ─────────────────────────────

    // Public, but self-closing: it only works while no user exists. That is
    // what lets a fresh install create its first admin without shipping a
    // default password, and what stops it being a permanent open door.
    srv.Post("/api/auth/bootstrap", [this](const httplib::Request& req,
                                           httplib::Response& res) {
        if (users_.count() > 0)
            return json_error(res, 409, "Already initialised — ask an admin for an account");

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return json_error(res, 400, "Request body must be JSON"); }

        const std::string username = body.value("username", "");
        const std::string password = body.value("password", "");
        const std::string display  = body.value("display_name", username);

        if (username.empty() || password.empty())
            return json_error(res, 400, "'username' and 'password' are required");
        if (password.size() < 8)
            return json_error(res, 400, "Password must be at least 8 characters");

        int64_t id = users_.create_user(username, password, display, UserStore::ROLE_ADMIN);
        if (id == 0) return json_error(res, 500, "Could not create the admin account");

        std::cerr << "[auth] bootstrapped admin account '" << username << "'\n";

        // Re-check the count rather than trusting the insert: if two bootstrap
        // requests raced, exactly one created a user and the other must not
        // hand out a session for it.
        std::string token = users_.create_token(id, SESSION_TTL_DAYS);
        if (token.empty()) return json_error(res, 500, "Could not start a session");

        res.set_header("Set-Cookie", session_cookie(token, SESSION_TTL_DAYS * 86400));
        json_reply(res, 200, {{"ok", true}, {"user", {{"id", id},
                                                      {"username", username},
                                                      {"display_name", display},
                                                      {"role", UserStore::ROLE_ADMIN}}}});
    });

    srv.Post("/api/login", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        try { body = json::parse(req.body); }
        catch (...) { return json_error(res, 400, "Request body must be JSON"); }

        const std::string username = body.value("username", "");
        const std::string password = body.value("password", "");

        auto user = users_.verify_login(username, password);
        if (!user) {
            // One message for both "no such user" and "wrong password", so the
            // endpoint can't be used to enumerate accounts.
            std::cerr << "[auth] failed login for '" << username << "' from "
                      << req.remote_addr << "\n";
            return json_error(res, 401, "Invalid username or password");
        }

        std::string token = users_.create_token(user->id, SESSION_TTL_DAYS);
        if (token.empty()) return json_error(res, 500, "Could not start a session");

        std::cerr << "[auth] login: " << user->username << " from " << req.remote_addr << "\n";
        res.set_header("Set-Cookie", session_cookie(token, SESSION_TTL_DAYS * 86400));
        json_reply(res, 200, {{"ok", true}, {"user", {{"id", user->id},
                                                      {"username", user->username},
                                                      {"display_name", user->display_name},
                                                      {"role", user->role}}}});
    });

    srv.Post("/api/logout", [this](const httplib::Request& req, httplib::Response& res) {
        // Revoke server-side as well as clearing the cookie: a cookie the
        // client merely forgets is still a valid credential to anyone who
        // captured it.
        const std::string token = read_cookie(req, COOKIE_NAME);
        if (!token.empty()) users_.revoke_token(token);
        res.set_header("Set-Cookie", session_cookie("", 0));
        json_reply(res, 200, {{"ok", true}});
    });

    // What the UI asks before rendering anything: does this install need a
    // first admin, and am I already logged in?
    srv.Get("/api/auth/status", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = authenticate(req);
        json out = {{"ok", true},
                    {"needs_bootstrap", users_.count() == 0},
                    {"authenticated", user.has_value()}};
        if (user)
            out["user"] = {{"id", user->id}, {"username", user->username},
                           {"display_name", user->display_name}, {"role", user->role}};
        json_reply(res, 200, out);
    });

    // ── status ────────────────────────────────────────────────────────────────
    srv.Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;
        // The model name is what the UI shows, so everyone gets it. The URL
        // and provider describe the operator's infrastructure — an internal
        // hostname and port a member has no use for and should not be handed.
        json llm = {{"model", defaults_.llm_model.empty() ? "default" : defaults_.llm_model}};
        if (user->is_admin()) {
            llm["url"]      = defaults_.llm_url;
            llm["provider"] = defaults_.llm_provider;
        }
        json_reply(res, 200, {
            {"ok",       true},
            {"name",     "funes"},
            {"version",  "4.0.0"},
            {"agents",   agent_count()},
            {"memories", memory_.count(user->id)},
            {"semantic_memory", memory_.semantic_available()},
            {"llm", llm}
        });
    });

    // ── agents ────────────────────────────────────────────────────────────────
    srv.Get("/api/agents", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;

        const auto perms = funes::Permissions::parse(user->permissions, user->is_admin());
        json arr = json::array();
        {
            std::lock_guard<std::mutex> lock(agents_mu_);
            for (const auto& [name, cfg] : agents_) {
                if (!perms.allows_agent(name)) continue;
                arr.push_back({
                    {"name",        name},
                    {"description", cfg.description},
                    {"tools",       cfg.tools},
                    {"is_default",  name == default_agent_}
                });
            }
        }
        json_reply(res, 200, {{"ok", true}, {"agents", arr}});
    });

    // Admin-only: the reload replaces the agent map for every session on the
    // box, not just the caller's, so it is not a member's call to make.
    srv.Post("/api/agents/reload", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_admin(req, res);
        if (!user) return;

        size_t n = load_agents();
        json_reply(res, 200, {{"ok", true}, {"agents", n}});
    });

    // ── chat (SSE) ────────────────────────────────────────────────────────────
    srv.Post("/api/chat", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return json_error(res, 400, "Request body must be JSON"); }

        const std::string message = body.value("message", "");
        const std::string session = body.value("session", "");
        const std::string agent_name = body.value("agent", "");

        std::vector<ImageAttachment> images;
        if (body.contains("images")) {
            if (!body["images"].is_array())
                return json_error(res, 400, "'images' must be an array");
            if (body["images"].size() > MAX_IMAGES_PER_MESSAGE)
                return json_error(res, 400, "Too many images (max " +
                                  std::to_string(MAX_IMAGES_PER_MESSAGE) + ")");
            for (const auto& img : body["images"]) {
                const std::string mime = img.value("mime_type", "");
                if (!img.is_object() || mime.rfind("image/", 0) != 0
                    || !img.contains("data") || !img["data"].is_string())
                    return json_error(res, 400,
                        "Each image needs a 'mime_type' starting with image/ and a base64 'data' string");
                std::string data = img["data"].get<std::string>();
                if (data.size() > MAX_IMAGE_BASE64_BYTES)
                    return json_error(res, 400, "Image too large (max ~6 MB)");
                images.push_back({mime, std::move(data)});
            }
        }

        if (message.empty() && images.empty())
            return json_error(res, 400, "'message' or 'images' is required");
        if (message.size() > MAX_MESSAGE_BYTES)
            return json_error(res, 400, "'message' too long (max 16 KB)");
        if (!valid_session(session))
            return json_error(res, 400, "'session' must match [A-Za-z0-9_-]{1,64}");

        AgentConfig cfg = find_agent(agent_name);
        if (cfg.name.empty())
            return json_error(res, 404, "Unknown agent: " + agent_name);

        const auto perms = funes::Permissions::parse(user->permissions, user->is_admin());
        if (!perms.allows_agent(cfg.name)) {
            std::cerr << "[auth] " << user->username << " denied agent '"
                      << cfg.name << "' by permissions\n";
            return json_error(res, 403, "You do not have access to the agent '" +
                              cfg.name + "'");
        }

        // Shared state for the provider lambda (it must be copyable).
        struct ChatJob {
            AgentConfig   cfg;
            std::string   message, session;
            std::vector<ImageAttachment> images;
            FunesApi*     api;
            int64_t       user_id;
            funes::Permissions perms;
        };
        auto job = std::make_shared<ChatJob>(
            ChatJob{std::move(cfg), message, session, std::move(images), this,
                    user->id, perms});

        res.set_header("Cache-Control", "no-store");
        res.set_chunked_content_provider("text/event-stream",
            [job](size_t /*offset*/, httplib::DataSink& sink) {
                std::atomic<bool> cancelled{false};
                auto emit = [&sink, &cancelled](const std::string& type, const json& data) {
                    if (cancelled.load(std::memory_order_relaxed)) return;
                    if (!sse_write(sink, type, data))
                        cancelled.store(true, std::memory_order_relaxed);
                };

                funes::cancel_flag() = &cancelled;
                try {
                    FunesAgent agent(job->cfg, job->api->tools_, job->api->memory_,
                                     job->api->defaults_);
                    std::string answer = agent.run(job->message, job->session,
                                                   job->user_id, job->perms,
                                                   emit, job->images);
                    emit("done", {{"text", answer}});
                } catch (const std::exception& e) {
                    emit("error", {{"message", e.what()}});
                }
                funes::cancel_flag() = nullptr;

                sink.done();
                return true;
            });
    });

    // ── memories ──────────────────────────────────────────────────────────────
    srv.Get("/api/memories", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;

        const std::string agent = req.get_param_value("agent");
        const std::string q     = req.get_param_value("q");
        int limit  = 50;
        int offset = 0;
        if (req.has_param("limit"))  limit  = std::atoi(req.get_param_value("limit").c_str());
        if (req.has_param("offset")) offset = std::atoi(req.get_param_value("offset").c_str());
        if (limit < 1 || limit > 200) limit = 50;
        if (offset < 0) offset = 0;

        std::vector<MemoryStore::Memory> items = q.empty()
            ? memory_.list(user->id, agent, limit, offset)
            // touch=false: browsing the memory list is not a recall, and
            // counting it would shield memories from consolidation's prune.
            : memory_.recall(user->id, agent, q, limit, /*touch=*/false);

        json arr = json::array();
        for (const auto& m : items) {
            arr.push_back({
                {"id",         m.id},
                {"agent",      m.agent},
                {"text",       m.text},
                {"source",     m.source},
                {"score",      m.score},
                {"created_at", m.created_at},
                // Consolidation prunes on this (never-recalled auto memories),
                // so it's worth being able to see it from outside.
                {"recall_count", m.recall_count}
            });
        }
        json_reply(res, 200, {{"ok", true}, {"memories", arr},
                              {"total", memory_.count(user->id, agent)},
                              {"semantic", memory_.semantic_available()}});
    });

    srv.Post("/api/memories", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;

        json body;
        try { body = json::parse(req.body); }
        catch (...) { return json_error(res, 400, "Request body must be JSON"); }

        const std::string text = body.value("text", "");
        std::string agent      = body.value("agent", "");
        if (agent.empty()) agent = default_agent_;

        if (text.empty())
            return json_error(res, 400, "'text' is required");
        if (text.size() > MAX_MEMORY_BYTES)
            return json_error(res, 400, "'text' too long (max 4 KB)");
        if (find_agent(agent).name.empty())
            return json_error(res, 404, "Unknown agent: " + agent);

        int64_t id = memory_.remember(user->id, agent, text, "user");
        json_reply(res, 200, {{"ok", true}, {"id", id}});
    });

    srv.Delete(R"(/api/memories/(\d+))", [this](const httplib::Request& req,
                                                httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;

        int64_t id = std::stoll(req.matches[1].str());
        if (!memory_.forget(user->id, id))
            return json_error(res, 404, "No memory with id " + std::to_string(id));
        json_reply(res, 200, {{"ok", true}});
    });

    // ── history (restore a session's chat on page reload) ─────────────────────
    srv.Get("/api/history", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;

        const std::string session = req.get_param_value("session");
        if (!valid_session(session))
            return json_error(res, 400, "'session' must match [A-Za-z0-9_-]{1,64}");
        int limit = 50;
        if (req.has_param("limit")) limit = std::atoi(req.get_param_value("limit").c_str());
        if (limit < 1 || limit > 200) limit = 50;

        json arr = json::array();
        for (const auto& turn : memory_.recent_turns(user->id, session, limit))
            arr.push_back({{"role", turn.role}, {"content", turn.content}});
        json_reply(res, 200, {{"ok", true}, {"turns", arr}});
    });

    // ── sessions (the UI's conversation list) ──────────────────────────────────
    srv.Get("/api/sessions", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;

        int limit = 50;
        if (req.has_param("limit")) limit = std::atoi(req.get_param_value("limit").c_str());
        if (limit < 1 || limit > 200) limit = 50;

        json arr = json::array();
        for (const auto& s : memory_.list_sessions(user->id, limit)) {
            arr.push_back({
                {"session",         s.session},
                {"last_message_at", s.last_message_at},
                {"preview",         s.preview},
                {"turn_count",      s.turn_count}
            });
        }
        json_reply(res, 200, {{"ok", true}, {"sessions", arr}});
    });

    // ── cron jobs (read-only view for the UI; managed via the operator agent's
    // schedule_job/cancel_job/run_job_now tools — see core/tools/cron_tool.cpp
    // and core/cron_runner.h for what actually runs a due job) ────────────────
    srv.Get("/api/jobs", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;

        json arr = json::array();
        for (const auto& j : memory_.list_cron_jobs(user->id)) {
            arr.push_back({
                {"id",          j.id},
                {"name",        j.name},
                {"kind",        j.kind},
                {"agent",       j.agent},
                {"task",        j.task},
                {"command",     j.command},
                {"schedule",    j.schedule},
                {"running",     j.running},
                {"next_run_at", j.next_run_at},
                {"last_run_at", j.last_run_at},
                {"last_status", j.last_status},
                {"last_output", j.last_output}
            });
        }
        json_reply(res, 200, {{"ok", true}, {"jobs", arr}});
    });

    // ── upload (attach a file to the chat from the UI) ────────────────────────
    srv.Post("/api/upload", [this](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;

        if (!req.has_file("file"))
            return json_error(res, 400, "Missing 'file' field (multipart/form-data)");

        const auto file = req.get_file_value("file");
        if (file.content.size() > MAX_UPLOAD_BYTES)
            return json_error(res, 400, "File too large (max 5 MB)");

        std::string safe_name = fs::path(file.filename).filename().string();
        if (safe_name.empty() || safe_name == "." || safe_name == "..")
            safe_name = "upload";
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        safe_name = std::to_string(stamp) + "_" + safe_name;

        // The uploader's own workspace, not the shared root — otherwise an
        // attachment would be readable by every account's read_file, and the
        // path handed back would resolve outside the caller's confinement.
        const fs::path workspace =
            funes::fsguard::workspace_for(workspace_dir_, user->id, "");
        fs::path dest = workspace / safe_name;

        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) return json_error(res, 500, "Could not save upload");
        out << file.content;
        out.close();

        const std::string image_mime = funes::detect_image_mime(file.content);
        if (!image_mime.empty()) {
            return json_reply(res, 200, {
                {"ok", true},
                {"filename", safe_name},
                {"path", dest.string()},
                {"size", file.content.size()},
                {"is_text", false},
                {"is_image", true},
                {"mime_type", image_mime},
                {"data", funes::base64_encode(file.content)},
                {"content", ""},
                {"truncated", false}
            });
        }

        bool is_text = false;
        std::string preview;
        bool truncated = false;

        if (looks_like_pdf(file.content)) {
            funes::pdf::ExtractResult extracted = funes::pdf::extract_text(
                dest, workspace, PDF_UPLOAD_TIMEOUT_S, MAX_UPLOAD_PREVIEW);
            is_text = extracted.ok;
            if (extracted.ok) preview = extracted.text_or_error;
        } else if (funes::looks_like_text(file.content)) {
            is_text = true;
            preview = file.content;
            if (preview.size() > MAX_UPLOAD_PREVIEW) {
                funes::truncate_utf8_safe(preview, MAX_UPLOAD_PREVIEW);
                truncated = true;
            }
        }

        json_reply(res, 200, {
            {"ok", true},
            {"filename", safe_name},
            {"path", dest.string()},
            {"size", file.content.size()},
            {"is_text", is_text},
            {"is_image", false},
            {"content", preview},
            {"truncated", truncated}
        });
    });

    // ── static UI ─────────────────────────────────────────────────────────────
    if (!ui_dir_.empty() && fs::exists(ui_dir_)) {
        srv.set_mount_point("/", ui_dir_);
    } else {
        srv.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("Funes is running, but the ui/ directory was not found. "
                            "Set FUNES_UI_DIR or run from the project root.",
                            "text/plain");
        });
    }
}
