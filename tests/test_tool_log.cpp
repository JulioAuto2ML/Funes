// =============================================================================
// tests/test_tool_log.cpp — the per-tool-call log line
// =============================================================================
// What the journal says about each tool call. The properties that matter: the
// outcome is distinguishable (error vs. budget vs. permissions — the 2026-10-21
// `home` incident was exactly not being able to tell those apart), the line is
// one line and bounded, and a credential in the arguments never reaches it.

#include "tool_log.h"
#include <iostream>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

using nlohmann::json;
using funes::ToolCallOutcome;

static bool mentions(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

static size_t count(const std::string& s, char c) {
    size_t n = 0;
    for (char x : s) n += (x == c);
    return n;
}

int main() {
    // ── argument summary ─────────────────────────────────────────────────────
    {
        const std::string s = funes::summarize_tool_args({{"name", "tv_art"}});
        CHECK(s == R"({"name":"tv_art"})");
    }
    // Credentials are redacted by key, at any depth and in any case — and the
    // value never appears, not even as a prefix.
    {
        const json args = {
            {"url", "https://example.com"},
            {"api_key", "sk-live-abcdef"},
            {"headers", {{"Authorization", "Bearer xyz123"}}},
            {"env", json::array({{{"GITHUB_TOKEN", "ghp_secretvalue"}}})},
            {"Password", "hunter2"},
        };
        const std::string s = funes::summarize_tool_args(args, 1000);
        CHECK(!mentions(s, "sk-live"));
        CHECK(!mentions(s, "xyz123"));
        CHECK(!mentions(s, "ghp_"));
        CHECK(!mentions(s, "hunter2"));
        CHECK(mentions(s, "***"));
        CHECK(mentions(s, "example.com"));   // only credentials go
    }
    // Bounded, marked when cut, and never split inside a UTF-8 sequence.
    {
        const std::string s = funes::summarize_tool_args({{"task", std::string(500, 'x')}}, 120);
        CHECK(s.size() <= 120 + std::string("…").size());
        CHECK(mentions(s, "…"));
        const std::string u = funes::summarize_tool_args({{"t", "ñññññññññññññññ"}}, 10);
        const std::string body = u.substr(0, u.size() - std::string("…").size());
        CHECK(json(body).dump(-1, ' ', false, json::error_handler_t::strict).size() > 0);
    }
    // A newline in an argument is escaped by the dump, so the summary is one line.
    CHECK(count(funes::summarize_tool_args({{"cmd", "a\nb"}}), '\n') == 0);

    // ── the line ─────────────────────────────────────────────────────────────
    {
        const std::string l = funes::format_tool_call_line(
            "home", 1, "run_script:tv_art", R"({"name":"tv_art"})", ToolCallOutcome::Ok);
        CHECK(l == "[tool] home (depth 1) run_script:tv_art args={\"name\":\"tv_art\"} → ok\n");
    }
    // Each outcome reads differently — that is the point of the line.
    {
        const std::string err = funes::format_tool_call_line(
            "home", 1, "schedule_job", "{}", ToolCallOutcome::Error,
            "invalid cron\nexpression: '45 6-23'");
        CHECK(mentions(err, "→ error: invalid cron expression"));
        CHECK(count(err, '\n') == 1);   // only the terminator

        const std::string budget = funes::format_tool_call_line(
            "home", 1, "schedule_job", "{}", ToolCallOutcome::BudgetRefused,
            "schedule_job limit 2");
        CHECK(mentions(budget, "→ refused: budget spent (schedule_job limit 2)"));

        const std::string perm = funes::format_tool_call_line(
            "home", 0, "execute_shell", "{}", ToolCallOutcome::PermissionDenied,
            "execute_shell");
        CHECK(mentions(perm, "→ refused: permissions (execute_shell)"));
        CHECK(mentions(perm, "(depth 0)"));

        // An identical repeat refused by the loop (tool_budget.h, repeat_action)
        // is its own outcome, not an "error" — the tool never ran.
        const std::string rep = funes::format_tool_call_line(
            "funes", 0, "recall", "{}", ToolCallOutcome::RepeatRefused, "3x");
        CHECK(mentions(rep, "→ refused: identical repeat (3x)"));

        // Ok ignores detail: a success's result text is not log material.
        const std::string ok = funes::format_tool_call_line(
            "home", 0, "web_fetch", "{}", ToolCallOutcome::Ok, "<html>…page…</html>");
        CHECK(!mentions(ok, "html"));
    }
    // A long error is cut to the detail bound.
    {
        const std::string l = funes::format_tool_call_line(
            "a", 0, "t", "{}", ToolCallOutcome::Error, std::string(1000, 'e'), 160);
        CHECK(count(l, 'e') <= 160 + 2);   // + the 'e' in "error:" ... and none else
    }

    std::cout << "tool_log: all checks passed\n";
    return 0;
}
