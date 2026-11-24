// =============================================================================
// src/server/practice_api.h — /api/practice/* (the practice view's routes)
// =============================================================================
//   GET  /api/practice/library          — {voices, exercises, errors}
//   GET  /api/practice/profile          — {profile | null}: the caller's range
//   PUT  /api/practice/profile          — {voice} for a default voice, or
//                                         {full: [low, high], source: "measured"}
//   GET  /api/practice/exercises/<id>   — the exercise fitted to the caller's
//                                         range: {exercise, range, reps}
//   GET  /api/practice/plan             — {plan | null}: the coach's plan for today
//   POST /api/practice/sessions         — {exercise, scores, ...} → appended
//
// What a person sings never comes here: the browser detects the pitch and
// sends only the scores. A profile and the session log are files in the
// caller's workspace (practice_store.h: profile.json, sessions.jsonl, plan.json) — per
// account like every workspace, and readable by the coach agent later with
// read_file, which is the point of putting them there rather than in a table.
// Since a model can also write_file there, every read is validated again.
// =============================================================================

#pragma once
#include "users.h"
#include <functional>
#include <optional>
#include <string>

namespace httplib { class Server; class Request; class Response; }

namespace funes::practice_api {

using Auth = std::function<std::optional<UserStore::User>(const httplib::Request&,
                                                          httplib::Response&)>;

// `practice_dir` is the library (practice/), read on every request so an
// edited exercise needs no restart; `workspace_dir` the workspaces' root.
void mount(httplib::Server& srv, Auth require_auth,
           const std::string& practice_dir, const std::string& workspace_dir);

} // namespace funes::practice_api
