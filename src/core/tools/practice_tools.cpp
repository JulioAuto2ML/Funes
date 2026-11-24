// =============================================================================
// src/core/tools/practice_tools.cpp — the coach's tools (singing practice, 7.25)
// =============================================================================
// practice_exercises, practice_history and practice_plan: what the vocal-coach
// agent knows about a person's practice, and its one way to act on it. The
// numbers come from the browser (the Practice view scores every repetition)
// and are summarised here; the model is asked only for what needs judgment —
// which exercises today, and two or three things to say about the results.
//
// The plan is the same rule the library follows: a model never writes notes,
// and it never invents an exercise either. practice_plan takes ids, checks
// each against the library (an unknown id is refused by name), caps the
// repetitions at what fits the person's range, and writes plan.json, which
// the Practice view shows as today's list. All three read and write only the
// caller's own workspace (practice_store.h), so they are own data
// (memory_provenance.cpp) and the plan is a workspace effect (effects.cpp).
// =============================================================================

#include "../practice_store.h"
#include "../tools.h"
#include <algorithm>
#include <ctime>
#include <iostream>

namespace {

using namespace funes::practice;

std::string since_days(int days) {
    std::time_t t = std::time(nullptr) - static_cast<std::time_t>(days) * 86400;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

} // namespace

void register_practice_tools(ToolRegistry& reg, const std::string& workspace_dir,
                             const std::string& practice_dir,
                             std::function<std::string(int64_t)> locale_for) {
    auto locale = [locale_for](int64_t uid) {
        std::string l = locale_for ? locale_for(uid) : "";
        return l.empty() ? std::string("en") : l.substr(0, 2);
    };

    reg.add({
        "practice_exercises",
        "The singing exercises Funes has (the only ones that exist), with their ids, "
        "category, level, what each one measures, and how many repetitions fit the "
        "person's vocal range. Use it before planning a session.",
        {{"type", "object"}, {"properties", json::object()}},
        [=](const json&, const ToolContext& ctx) -> ToolResult {
            auto lib = load_library(practice_dir);
            if (lib.exercises.empty()) return {"The exercise library is empty or missing.", true};
            auto p = load_profile(user_dir(workspace_dir, ctx.user_id));
            return {exercises_report(lib, p ? &*p : nullptr, locale(ctx.user_id))};
        },
        /*inline_result=*/true
    });

    reg.add({
        "practice_history",
        "What the person has practised and how it went, as the browser measured it: per "
        "exercise how often, the last scores and the trend, and the notes they most often "
        "sing flat or sharp; also what they have not practised and their vocal range.",
        {
            {"type", "object"},
            {"properties", {
                {"days", {{"type", "integer"},
                          {"description", "How many days back (default 14, max 120)"}}}
            }}
        },
        [=](const json& args, const ToolContext& ctx) -> ToolResult {
            int days = 14;
            if (args.contains("days") && args["days"].is_number())
                days = std::clamp(static_cast<int>(args["days"].get<double>()), 1, 120);
            const auto dir = user_dir(workspace_dir, ctx.user_id);
            auto lib = load_library(practice_dir);
            auto p = load_profile(dir);
            return {history_report(load_sessions(dir), lib, p ? &*p : nullptr,
                                   since_days(days), locale(ctx.user_id))};
        },
        /*inline_result=*/true
    });

    reg.add({
        "practice_plan",
        "Set today's practice plan, which the person sees in Funes's Practice view as a "
        "list to work through in order. Only exercise ids from practice_exercises; reps "
        "optional (capped at what fits the range). `note`: one or two sentences on what "
        "today is for, in the person's language. Replaces the previous plan.",
        {
            {"type", "object"},
            {"properties", {
                {"items", {
                    {"type", "array"},
                    {"description", "The exercises in order, warm-up first"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"id", {{"type", "string"}}},
                            {"reps", {{"type", "integer"}}}
                        }},
                        {"required", {"id"}}
                    }}
                }},
                {"note", {{"type", "string"}, {"description", "What today's session is for (≤600 characters)"}}}
            }},
            {"required", {"items"}}
        },
        [=](const json& args, const ToolContext& ctx) -> ToolResult {
            const auto dir = user_dir(workspace_dir, ctx.user_id);
            auto lib = load_library(practice_dir);
            auto p = load_profile(dir);
            json plan;
            if (std::string err = make_plan(args, lib, p ? &*p : nullptr, now_iso(), plan); !err.empty())
                return {"Plan not saved: " + err, true};
            plan["by"] = ctx.agent;
            if (!write_json(dir / "plan.json", plan)) return {"Plan not saved: could not write it.", true};
            std::cerr << "[practice] plan for user " << ctx.user_id << ": " << plan["items"].dump() << "\n";
            std::string list;
            for (const auto& it : plan["items"])
                list += "\n- " + it["id"].get<std::string>() + " × " + std::to_string(it["reps"].get<int>());
            return {"Saved. The person sees this plan in the Practice view (Práctica):" + list};
        }
    });
}
