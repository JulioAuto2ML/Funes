// =============================================================================
// src/server/practice_api.cpp — see practice_api.h
// =============================================================================

#include "practice_api.h"
#include "practice_store.h"
#include "tools/fs_guard.h"
#include "httplib.h"
#include "json.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace funes::practice_api {

namespace {

// One session's scores: a few dozen notes with a handful of numbers each.
constexpr size_t kMaxSessionBytes = 64 * 1024;

void reply(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void error(httplib::Response& res, int status, const std::string& message) {
    reply(res, status, {{"ok", false}, {"error", message}});
}

const practice::Exercise* find(const practice::Library& lib, const std::string& id) {
    for (const auto& e : lib.exercises)
        if (e.id == id) return &e;
    return nullptr;
}

const practice::Voice* find_voice(const practice::Library& lib, const std::string& name) {
    for (const auto& v : lib.voices)
        if (v.name == name) return &v;
    return nullptr;
}

} // namespace

void mount(httplib::Server& srv, Auth require_auth,
           const std::string& practice_dir, const std::string& workspace_dir) {

    srv.Get("/api/practice/library", [=](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;
        auto lib = practice::load_library(practice_dir);
        for (const auto& err : lib.errors) std::cerr << "[practice] " << err << "\n";
        json voices = json::array();
        for (const auto& v : lib.voices) voices.push_back(profile_to_json(practice::profile_for(v)));
        json exercises = json::array();
        for (const auto& e : lib.exercises) exercises.push_back(e.to_json());
        reply(res, 200, {{"ok", true}, {"voices", voices}, {"exercises", exercises},
                         {"errors", lib.errors.size()}});
    });

    srv.Get("/api/practice/profile", [=](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;
        auto p = practice::load_profile(practice::user_dir(workspace_dir, user->id));
        reply(res, 200, {{"ok", true}, {"profile", p ? profile_to_json(*p) : json(nullptr)}});
    });

    srv.Put("/api/practice/profile", [=](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;
        json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) return error(res, 400, "A JSON object is required");

        practice::Profile p;
        if (body.contains("full")) {
            if (std::string err = practice::profile_from_json(body, p); !err.empty())
                return error(res, 400, err);
            if (p.source == "measured" && p.measured_at.empty()) p.measured_at = practice::now_iso();
        } else {
            auto lib = practice::load_library(practice_dir);
            const auto* v = find_voice(lib, body.value("voice", ""));
            if (!v) return error(res, 400, "Unknown voice type");
            p = practice::profile_for(*v);
        }
        const auto path = practice::user_dir(workspace_dir, user->id) / "profile.json";
        if (!practice::write_json(path, profile_to_json(p)))
            return error(res, 500, "Could not save the profile");
        std::cerr << "[practice] " << user->username << " range "
                  << practice::midi_name(p.full.low) << "-" << practice::midi_name(p.full.high)
                  << " (" << p.source << ")\n";
        reply(res, 200, {{"ok", true}, {"profile", profile_to_json(p)}});
    });

    srv.Get(R"(/api/practice/exercises/([a-z0-9_]{1,64}))",
            [=](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;
        auto lib = practice::load_library(practice_dir);
        const auto* e = find(lib, req.matches[1].str());
        if (!e) return error(res, 404, "No such exercise");
        auto p = practice::load_profile(practice::user_dir(workspace_dir, user->id));
        if (!p) return error(res, 409, "No vocal range yet: choose a voice type or measure it");
        json out = practice::plan(*e, *p);
        out["ok"] = true;
        reply(res, 200, out);
    });

    srv.Get("/api/practice/plan", [=](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;
        const auto dir = practice::user_dir(workspace_dir, user->id);
        auto plan = practice::load_plan(dir);
        if (plan) {
            // Which of its exercises have been sung since the plan was made.
            const std::string since = plan->value("created_at", "");
            json done = json::array();
            for (const auto& s : practice::load_sessions(dir, 200)) {
                const std::string id = s.value("exercise", "");
                if (s.value("at", "") >= since && !id.empty() &&
                    std::find(done.begin(), done.end(), id) == done.end())
                    done.push_back(id);
            }
            (*plan)["done"] = done;
        }
        reply(res, 200, {{"ok", true}, {"plan", plan ? *plan : json(nullptr)}});
    });

    srv.Post("/api/practice/sessions", [=](const httplib::Request& req, httplib::Response& res) {
        auto user = require_auth(req, res);
        if (!user) return;
        if (req.body.size() > kMaxSessionBytes) return error(res, 413, "Session too large");
        json body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) return error(res, 400, "A JSON object is required");
        const std::string id = body.value("exercise", "");
        auto lib = practice::load_library(practice_dir);
        if (!find(lib, id)) return error(res, 400, "Unknown exercise");
        if (!body.contains("scores") || !body["scores"].is_object())
            return error(res, 400, "scores is an object");
        body["at"] = practice::now_iso();

        if (!practice::append_session(practice::user_dir(workspace_dir, user->id), body))
            return error(res, 500, "Could not save the session");
        std::cerr << "[practice] " << user->username << " " << id << " "
                  << body["scores"].dump().substr(0, 200) << "\n";
        reply(res, 200, {{"ok", true}});
    });
}

} // namespace funes::practice_api
