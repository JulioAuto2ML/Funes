// =============================================================================
// src/server/api.cpp — Funes HTTP API implementation
// =============================================================================

#include "api.h"
#include "../core/base64.h"
#include "../core/text_utils.h"
#include "../core/tools/pdf_extract.h"
#include "httplib.h"
#include <chrono>
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

// Sniffs magic bytes rather than trusting the filename — returns the mime
// type for a recognized image format, or "" if `content` isn't one.
std::string detect_image_mime(const std::string& content) {
    auto starts_with = [&](size_t offset, std::initializer_list<unsigned char> bytes) {
        if (content.size() < offset + bytes.size()) return false;
        size_t i = offset;
        for (unsigned char b : bytes)
            if (static_cast<unsigned char>(content[i++]) != b) return false;
        return true;
    };

    if (starts_with(0, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return "image/png";
    if (starts_with(0, {0xFF, 0xD8, 0xFF}))                           return "image/jpeg";
    if (starts_with(0, {'G', 'I', 'F', '8', '7', 'a'})
        || starts_with(0, {'G', 'I', 'F', '8', '9', 'a'}))            return "image/gif";
    if (starts_with(0, {'R', 'I', 'F', 'F'}) && starts_with(8, {'W', 'E', 'B', 'P'}))
        return "image/webp";
    return "";
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

} // namespace

// ── construction / agents ─────────────────────────────────────────────────────

FunesApi::FunesApi(ToolRegistry& tools, MemoryStore& memory,
                   const AgentDefaults& defaults,
                   const std::string& agents_dir,
                   const std::string& ui_dir,
                   const std::string& default_agent,
                   const std::string& workspace_dir)
    : tools_(tools), memory_(memory), defaults_(defaults)
    , agents_dir_(agents_dir), ui_dir_(ui_dir), default_agent_(default_agent)
    , workspace_dir_(workspace_dir)
{
    load_agents();
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
    }
    return oss.str();
}

// ── routes ────────────────────────────────────────────────────────────────────

void FunesApi::mount(httplib::Server& srv) {

    // ── status ────────────────────────────────────────────────────────────────
    srv.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        json_reply(res, 200, {
            {"ok",       true},
            {"name",     "funes"},
            {"version",  "2.0.0"},
            {"agents",   agent_count()},
            {"memories", memory_.count()},
            {"semantic_memory", memory_.semantic_available()},
            {"llm", {
                {"url",      defaults_.llm_url},
                {"provider", defaults_.llm_provider},
                {"model",    defaults_.llm_model.empty() ? "default" : defaults_.llm_model}
            }}
        });
    });

    // ── agents ────────────────────────────────────────────────────────────────
    srv.Get("/api/agents", [this](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        {
            std::lock_guard<std::mutex> lock(agents_mu_);
            for (const auto& [name, cfg] : agents_) {
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

    srv.Post("/api/agents/reload", [this](const httplib::Request&, httplib::Response& res) {
        size_t n = load_agents();
        json_reply(res, 200, {{"ok", true}, {"agents", n}});
    });

    // ── chat (SSE) ────────────────────────────────────────────────────────────
    srv.Post("/api/chat", [this](const httplib::Request& req, httplib::Response& res) {
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

        // Shared state for the provider lambda (it must be copyable).
        struct ChatJob {
            AgentConfig   cfg;
            std::string   message, session;
            std::vector<ImageAttachment> images;
            FunesApi*     api;
        };
        auto job = std::make_shared<ChatJob>(
            ChatJob{std::move(cfg), message, session, std::move(images), this});

        res.set_header("Cache-Control", "no-store");
        res.set_chunked_content_provider("text/event-stream",
            [job](size_t /*offset*/, httplib::DataSink& sink) {
                bool client_gone = false;
                auto emit = [&sink, &client_gone](const std::string& type, const json& data) {
                    if (client_gone) return;
                    if (!sse_write(sink, type, data)) client_gone = true;
                };

                try {
                    FunesAgent agent(job->cfg, job->api->tools_, job->api->memory_,
                                     job->api->defaults_);
                    std::string answer = agent.run(job->message, job->session, emit, job->images);
                    emit("done", {{"text", answer}});
                } catch (const std::exception& e) {
                    emit("error", {{"message", e.what()}});
                }

                sink.done();
                return true;
            });
    });

    // ── memories ──────────────────────────────────────────────────────────────
    srv.Get("/api/memories", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string agent = req.get_param_value("agent");
        const std::string q     = req.get_param_value("q");
        int limit  = 50;
        int offset = 0;
        if (req.has_param("limit"))  limit  = std::atoi(req.get_param_value("limit").c_str());
        if (req.has_param("offset")) offset = std::atoi(req.get_param_value("offset").c_str());
        if (limit < 1 || limit > 200) limit = 50;
        if (offset < 0) offset = 0;

        std::vector<MemoryStore::Memory> items = q.empty()
            ? memory_.list(agent, limit, offset)
            // touch=false: browsing the memory list is not a recall, and
            // counting it would shield memories from consolidation's prune.
            : memory_.recall(agent, q, limit, /*touch=*/false);

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
                              {"total", memory_.count(agent)},
                              {"semantic", memory_.semantic_available()}});
    });

    srv.Post("/api/memories", [this](const httplib::Request& req, httplib::Response& res) {
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

        int64_t id = memory_.remember(agent, text, "user");
        json_reply(res, 200, {{"ok", true}, {"id", id}});
    });

    srv.Delete(R"(/api/memories/(\d+))", [this](const httplib::Request& req,
                                                httplib::Response& res) {
        int64_t id = std::stoll(req.matches[1].str());
        if (!memory_.forget(id))
            return json_error(res, 404, "No memory with id " + std::to_string(id));
        json_reply(res, 200, {{"ok", true}});
    });

    // ── history (restore a session's chat on page reload) ─────────────────────
    srv.Get("/api/history", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string session = req.get_param_value("session");
        if (!valid_session(session))
            return json_error(res, 400, "'session' must match [A-Za-z0-9_-]{1,64}");
        int limit = 50;
        if (req.has_param("limit")) limit = std::atoi(req.get_param_value("limit").c_str());
        if (limit < 1 || limit > 200) limit = 50;

        json arr = json::array();
        for (const auto& turn : memory_.recent_turns(session, limit))
            arr.push_back({{"role", turn.role}, {"content", turn.content}});
        json_reply(res, 200, {{"ok", true}, {"turns", arr}});
    });

    // ── sessions (the UI's conversation list) ──────────────────────────────────
    srv.Get("/api/sessions", [this](const httplib::Request& req, httplib::Response& res) {
        int limit = 50;
        if (req.has_param("limit")) limit = std::atoi(req.get_param_value("limit").c_str());
        if (limit < 1 || limit > 200) limit = 50;

        json arr = json::array();
        for (const auto& s : memory_.list_sessions(limit)) {
            arr.push_back({
                {"session",         s.session},
                {"last_message_at", s.last_message_at},
                {"preview",         s.preview},
                {"turn_count",      s.turn_count}
            });
        }
        json_reply(res, 200, {{"ok", true}, {"sessions", arr}});
    });

    // ── upload (attach a file to the chat from the UI) ────────────────────────
    srv.Post("/api/upload", [this](const httplib::Request& req, httplib::Response& res) {
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

        std::error_code ec;
        fs::create_directories(workspace_dir_, ec);
        fs::path dest = fs::path(workspace_dir_) / safe_name;

        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) return json_error(res, 500, "Could not save upload");
        out << file.content;
        out.close();

        const std::string image_mime = detect_image_mime(file.content);
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
                dest, workspace_dir_, PDF_UPLOAD_TIMEOUT_S, MAX_UPLOAD_PREVIEW);
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
