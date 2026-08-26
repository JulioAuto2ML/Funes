// =============================================================================
// src/server/main.cpp — the funes binary
// =============================================================================
//
// One process serves everything: the web UI, the REST/SSE API, the agent
// runtime, and the memory engine. Start it, open http://localhost:8484,
// and talk to something that remembers you.

#include "funes_config.h"
#include "agent.h"
#include "api.h"
#include "cron_runner.h"
#include "memory.h"
#include "tools.h"
#include "tools/cron_tool.h"
#include "tools/harvest.h"
#include "tools/http_tool_runtime.h"
#include "tools/issue.h"
#include "user_cli.h"
#include "users.h"
#include "httplib.h"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

// Resolve a data path: use `configured` if set, else try ./`relative` (cwd),
// else <exe_dir>/../`relative` (binary lives in bin/ under the project root).
//
// Always returns an absolute path. A bare relative string used to come back
// from the first branch, which was harmless for agents_dir/ui_dir/
// generated_tools_dir — read by this same process, whose cwd never changes —
// but broke publishing_dir: publish_issue embeds it into an argv for a
// subprocess spawned with cwd = the workspace directory (X_posts, not this
// process's cwd), so a relative "publishing" resolved against the wrong
// directory and python3 could not find the script. Found on 2026-07-31 via a
// live curator run that got all the way to `publish_issue.py exit 2: No such
// file or directory`.
static std::string resolve_dir(const std::string& configured, const std::string& relative) {
    if (!configured.empty()) return fs::absolute(configured).string();
    if (fs::exists(relative)) return fs::absolute(relative).string();

    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fs::path candidate = fs::path(buf).parent_path().parent_path() / relative;
        if (fs::exists(candidate)) return fs::absolute(candidate).string();
    }
    return fs::absolute(relative).string();
}

// The LLM half of MemoryStore::consolidate: given a cluster of near-identical
// memories, either one merged sentence or "KEEP ALL". The instruction is
// deliberately blunt and the reply is deliberately one line — this runs
// unattended against a local model, and anything it can hedge with ends up
// stored as a memory.
static std::string merge_memories(LLMClient& llm, const std::vector<std::string>& texts) {
    std::ostringstream oss;
    oss << "These notes were stored separately and look like near-duplicates:\n\n";
    for (const auto& t : texts) oss << "- " << t << "\n";
    oss << "\nIf they state the same fact, reply with ONE self-contained sentence "
           "that preserves every detail from all of them, and nothing else.\n"
           "If they are genuinely different facts, reply exactly: KEEP ALL";

    std::vector<ChatMessage> msgs;
    ChatMessage sys;
    sys.role    = "system";
    sys.content = "You merge duplicate notes. You reply with either the merged "
                  "sentence or the exact words KEEP ALL. No preamble, no "
                  "explanation, no quotes, no list.";
    msgs.push_back(std::move(sys));
    ChatMessage user;
    user.role    = "user";
    user.content = oss.str();
    msgs.push_back(std::move(user));

    std::string out = llm.complete(msgs).content;
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front())))
        out.erase(out.begin());
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
        out.pop_back();
    return out;
}

int main(int argc, char** argv) {
    funes::load_config();

    // ── configuration ─────────────────────────────────────────────────────────
    AgentDefaults defaults;
    defaults.llm_url         = funes::env("FUNES_LLM_URL", "http://localhost:8080");
    defaults.llm_api_key     = funes::env("FUNES_LLM_KEY");
    defaults.llm_provider    = funes::env("FUNES_LLM_PROVIDER", "openai");
    defaults.llm_model       = funes::env("FUNES_LLM_MODEL", "default");
    defaults.vision_url      = funes::env("FUNES_VISION_URL");
    defaults.memory_turns    = funes::env_int("FUNES_MEMORY_TURNS", 10);
    defaults.memory_recall_k = funes::env_int("FUNES_MEMORY_RECALL_K", 4);
    defaults.auto_memory     = funes::env("FUNES_AUTO_MEMORY", "1") != "0";

    const std::string host          = funes::env("FUNES_HOST", "127.0.0.1");
    const int         port          = funes::env_int("FUNES_PORT", 8484);
    const std::string default_agent = funes::env("FUNES_DEFAULT_AGENT", "funes");
    const std::string agents_dir    = resolve_dir(funes::env("FUNES_AGENTS_DIR"), "agents");
    const std::string ui_dir        = resolve_dir(funes::env("FUNES_UI_DIR"), "ui");
    const std::string generated_tools_dir = resolve_dir(
        funes::env("FUNES_GENERATED_TOOLS_DIR"), "src/core/tools/generated");
    // The publishing scripts publish_issue runs. In the repo, deployed by the
    // same `git pull` as the binary — see publishing/README.md.
    const std::string publishing_dir = resolve_dir(funes::env("FUNES_PUBLISHING_DIR"),
                                                   "publishing");
    // One YAML per publication: queries, windows, caps, artifacts, channels.
    const std::string publications_dir = resolve_dir(funes::env("FUNES_PUBLICATIONS_DIR"),
                                                     "publications");

    std::string db_path = funes::env("FUNES_DB");
    if (db_path.empty()) {
        const std::string home = funes::env("HOME", ".");
        std::error_code ec;
        fs::create_directories(home + "/.funes", ec);
        db_path = home + "/.funes/memory.db";
    }

    // Account management (`funes useradd` …) runs against the same database
    // and exits without starting the server. Dispatched here, after the config
    // and the database path are known but before anything opens a socket or
    // queries the LLM — a CLI invocation must not need a reachable model.
    if (funes::is_user_cli_command(argc, argv))
        return funes::run_user_cli(argc, argv, db_path);

    // read_file/write_file/execute_shell are confined to this directory.
    std::string workspace_dir = funes::env("FUNES_WORKSPACE_DIR");
    if (workspace_dir.empty()) {
        const std::string home = funes::env("HOME", ".");
        workspace_dir = home + "/.funes/workspace";
    }
    {
        std::error_code ec;
        fs::create_directories(workspace_dir, ec);
    }

    // With llama-server the model name is usually left as "default" — resolve
    // the real one so model-specific handling (e.g. Qwen tool-result format)
    // keys off the actual model, and the UI shows what is really running.
    if (defaults.llm_model == "default" && defaults.llm_provider == "openai") {
        std::string discovered = fetch_default_model(defaults.llm_url, defaults.llm_api_key);
        if (!discovered.empty()) {
            std::cerr << "[funes] LLM backend serves model: " << discovered << "\n";
            defaults.llm_model = discovered;
        }
    }

    // ── embedding client (optional — memory degrades to keyword search) ───────
    std::unique_ptr<EmbeddingClient> embedder;
    const std::string embed_url = funes::env("FUNES_EMBED_URL");
    if (!embed_url.empty()) {
        embedder = std::make_unique<EmbeddingClient>(
            embed_url,
            funes::env("FUNES_EMBED_KEY"),
            funes::env("FUNES_EMBED_MODEL", "default"));
    }

    // ── core services ─────────────────────────────────────────────────────────
    MemoryStore memory(db_path, embedder.get());
    UserStore   users(db_path);

    // A service token lets a non-browser caller (the WhatsApp autoresponder)
    // authenticate; it must still name a mapped jid to act as somebody. Unset
    // means service authentication is off entirely rather than open.
    const std::string service_token = funes::env("FUNES_SERVICE_TOKEN");
    if (service_token.empty())
        std::cerr << "[funes] FUNES_SERVICE_TOKEN not set — service callers "
                     "(WhatsApp autoresponder) cannot authenticate\n";
    else if (service_token.size() < 32)
        std::cerr << "[funes] warning: FUNES_SERVICE_TOKEN is shorter than 32 "
                     "characters; generate one with `openssl rand -hex 32`\n";

    ToolRegistry tools;
    register_web_tools(tools);
    register_memory_tools(tools, memory);
    register_result_tools(tools, memory);
    register_context_tools(tools, memory, defaults);
    register_introspection_tools(tools);
    register_file_tools(tools, workspace_dir);
    register_shell_tool(tools, workspace_dir);
    register_harvest_tool(tools, memory, workspace_dir, publications_dir);
    register_publish_issue_tool(tools, workspace_dir, publishing_dir,
                                publications_dir);
    funes::tools::register_all_generated_tools(tools);
    register_tool_builder(tools, generated_tools_dir);

    FunesApi api(tools, memory, users, defaults, agents_dir, ui_dir, default_agent,
                 workspace_dir, service_token);

    // Not a fatal condition — the UI's first-run screen calls
    // /api/auth/bootstrap to create this account — but it is worth saying out
    // loud, because until it happens every other endpoint answers 401 and
    // that looks like a broken deployment rather than an unfinished one.
    if (users.count() == 0)
        std::cerr << "[funes] no accounts yet — open the web UI to create the first "
                     "admin, or run: funes useradd <name> --admin\n";

    // Agents with the delegate_to_agent tool get their roster of other
    // agents (name + description) injected into their system prompt at
    // request time — see AgentDefaults::agent_roster in agent.h. This needs
    // `api` to exist (it owns the agent table), so it's wired up post-
    // construction and applied to both copies of `defaults` in play: the
    // one FunesApi already copied into itself (used for top-level turns),
    // and main's local one (used for delegated sub-agents below).
    auto agent_roster = [&api](const std::string& exclude) { return api.agent_roster(exclude); };
    api.set_agent_roster(agent_roster);
    defaults.agent_roster = agent_roster;

    // create_agent needs to trigger a live reload after writing a new agent
    // YAML, so it's wired up once FunesApi (which owns the agent table) exists.
    register_agent_builder(tools, agents_dir, [&api] { api.load_agents(); });

    // delegate_to_agent needs to look up other personas by name — also
    // wired up post-construction since FunesApi owns the agent table.
    register_delegation_tool(tools, memory, defaults,
        [&api](const std::string& name) { return api.find_agent(name); },
        [&api] { return api.agent_names(); });

    // schedule_job/run_job_now need the same agent lookup as delegation, for
    // kind="agent" jobs. See core/cron_runner.h for the poll loop and
    // publishing/README.md for the crontab this is meant to eventually replace.
    auto find_agent_for_cron = [&api](const std::string& name) { return api.find_agent(name); };
    register_cron_tool(tools, memory, defaults, workspace_dir, find_agent_for_cron);
    funes::cron::start_cron_runner(memory, tools, defaults, workspace_dir, find_agent_for_cron,
                                   funes::env_int("FUNES_CRON_POLL_SECONDS", 30));

    // Embed any memories that are missing vectors (e.g. stored while the
    // embedding endpoint was down) without blocking startup.
    std::thread([&memory] {
        size_t n = memory.backfill_embeddings();
        if (n > 0)
            std::cerr << "[funes] backfilled " << n << " memory embeddings\n";
    }).detach();

    // Stored tool results (see core/result_store.h) are only useful to the turn
    // that produced them; a startup sweep keeps the table from growing forever.
    {
        const int dropped = memory.prune_results_older_than(
            funes::env_int("FUNES_RESULT_TTL_DAYS", 7));
        if (dropped > 0)
            std::cerr << "[funes] pruned " << dropped << " stale tool results\n";
    }

    // Memory consolidation (see MemoryStore::consolidate): merge near-duplicate
    // memories, drop auto-captured ones that were never recalled. Sleeps first,
    // so a restart never costs an immediate LLM burst; FUNES_CONSOLIDATE=off
    // disables it entirely for debugging.
    if (funes::env("FUNES_CONSOLIDATE", "on") != "off") {
        MemoryStore::ConsolidationOptions opt;
        opt.prune_after_days = funes::env_int("FUNES_CONSOLIDATE_PRUNE_DAYS", 30);
        opt.max_clusters     = static_cast<size_t>(funes::env_int("FUNES_CONSOLIDATE_MAX_CLUSTERS", 20));
        const int every_hours = std::max(1, funes::env_int("FUNES_CONSOLIDATE_HOURS", 6));

        std::thread([&memory, &defaults, opt, every_hours] {
            LLMClient llm(defaults.llm_url, defaults.llm_api_key,
                          defaults.llm_model, defaults.llm_provider);
            llm.set_max_tokens(256);
            for (;;) {
                std::this_thread::sleep_for(std::chrono::hours(every_hours));
                try {
                    auto report = memory.consolidate(
                        [&llm](const std::vector<std::string>& texts) {
                            return merge_memories(llm, texts);
                        }, opt);
                    if (report.merged || report.pruned || report.clusters_seen)
                        std::cerr << "[funes] consolidation: " << report.clusters_seen
                                  << " cluster(s), merged " << report.merged
                                  << ", kept " << report.kept
                                  << ", pruned " << report.pruned << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "[funes] consolidation run failed: " << e.what() << "\n";
                }
            }
        }).detach();
    }

    // ── HTTP server ───────────────────────────────────────────────────────────
    httplib::Server srv;
    srv.set_read_timeout(60);
    srv.set_write_timeout(1200);   // SSE chat responses can be slow on local LLMs
    api.mount(srv);

    std::cout << "\n"
              << "  Funes — an assistant that remembers\n"
              << "  ───────────────────────────────────\n"
              << "  UI:        http://" << host << ":" << port << "\n"
              << "  LLM:       " << defaults.llm_url << " (" << defaults.llm_provider
              << ", model: " << defaults.llm_model << ")\n"
              << (defaults.vision_url.empty() ? ""
                  : "  Vision:    " + defaults.vision_url + "\n")
              << "  Memory:    " << db_path << " (" << memory.count(-1) << " memories, "
              << (embedder ? "semantic" : "keyword-only") << ")\n"
              << "  Agents:    " << api.agent_count() << " from " << agents_dir << "\n"
              << "  Workspace: " << workspace_dir << " (shell "
              << (funes::env("FUNES_ALLOW_SHELL", "0") == "1" ? "enabled" : "disabled") << ")\n"
              << "\n" << std::flush;

    signal(SIGPIPE, SIG_IGN);  // dropped SSE clients must not kill the process

    if (!srv.listen(host, port)) {
        std::cerr << "[funes] FATAL: cannot listen on " << host << ":" << port
                  << " (port in use?)\n";
        return 1;
    }
    return 0;
}
