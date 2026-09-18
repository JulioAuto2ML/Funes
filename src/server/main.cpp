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
#include "permissions.h"
#include "tools.h"
#include "tools/cron_tool.h"
#include "tools/harvest.h"
#include "tools/http_tool_runtime.h"
#include "tools/issue.h"
#include "tools/process_runner.h"
#include "mcp_stdio_client.h"
#include "maint_cli.h"
#include "user_cli.h"
#include "users.h"
#include "httplib.h"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
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

// 4.0: the workspace root gained a per-user level (<root>/<user_id>), so an
// existing install's files are sitting one directory too high and read_file
// would no longer find them. Move them once, into the admin's directory —
// the same account the memory migration attributes existing rows to.
//
// Guarded by a marker file rather than by "is the directory empty", because
// this moves a user's own files: getting it wrong twice is worse than not
// running it at all. Anything that fails to move is left where it is and
// named in the log, never silently dropped.
static void migrate_workspace_to_per_user(const fs::path& root) {
    const fs::path marker = root / ".per-user-layout";
    std::error_code ec;
    if (fs::exists(marker, ec)) return;

    const fs::path admin_dir = root / std::to_string(MemoryStore::ADMIN_USER_ID);

    std::vector<fs::path> to_move;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        const std::string name = entry.path().filename().string();
        if (name == admin_dir.filename().string()) continue;   // already there
        if (name == ".per-user-layout") continue;
        to_move.push_back(entry.path());
    }
    if (ec) {
        std::cerr << "[funes] cannot read workspace '" << root.string()
                  << "': " << ec.message() << " — skipping the per-user move\n";
        return;
    }

    if (!to_move.empty()) {
        fs::create_directories(admin_dir, ec);
        int moved = 0, failed = 0;
        for (const auto& src : to_move) {
            std::error_code move_ec;
            fs::rename(src, admin_dir / src.filename(), move_ec);
            if (move_ec) {
                ++failed;
                std::cerr << "[funes] could not move '" << src.string()
                          << "' into " << admin_dir.string() << ": "
                          << move_ec.message() << "\n";
            } else {
                ++moved;
            }
        }
        std::cerr << "[funes] workspace is now per-user: moved " << moved
                  << " entr" << (moved == 1 ? "y" : "ies") << " into "
                  << admin_dir.string();
        if (failed) std::cerr << " (" << failed << " left in place — see above)";
        std::cerr << "\n";
        // Leaving the marker unwritten after a partial failure would retry the
        // move next start, which is the right behaviour: the entries that did
        // move are already gone from the root, so a retry only sees the rest.
        if (failed) return;
    }

    std::ofstream(marker) << "Funes 4.0: user files live in <root>/<user_id>/\n";
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

// The LLM half of MemoryStore::backfill_links: one pair, one word back. The
// store has already decided this pair is worth asking about (same account,
// same agent, similar but not near-identical), so the only judgement left is
// the one arithmetic cannot make — whether knowing one of these helps when the
// other comes up.
//
// The closed vocabulary is the point. An open-ended "describe the
// relationship" from a 9B model unattended produces a different phrasing every
// time, and rel_type is a key other code groups on, not prose anybody reads.
static std::string judge_link(LLMClient& llm, const std::string& a, const std::string& b) {
    std::vector<ChatMessage> msgs;
    ChatMessage sys;
    sys.role    = "system";
    sys.content = "You decide whether two remembered notes are connected. Reply "
                  "with exactly one word: elaborates (one adds detail to the "
                  "other), causes (one explains why the other is true), "
                  "contradicts (they cannot both be true), related (connected "
                  "some other way), or none (knowing one does not help with the "
                  "other). One word. No explanation.";
    msgs.push_back(std::move(sys));
    ChatMessage user;
    user.role    = "user";
    user.content = "A: " + a + "\nB: " + b;
    msgs.push_back(std::move(user));

    std::string out = llm.complete(msgs).content;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Substring rather than equality: a local model that was told "one word"
    // still says "Related." often enough that treating it as a refusal would
    // throw away most of the pass.
    for (const char* verdict : {"elaborates", "contradicts", "causes", "related"})
        if (out.find(verdict) != std::string::npos) return verdict;
    return "";   // includes an explicit "none" and anything unparseable
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
    // One YAML per multi-stage pipeline: where each stage's entries go and what
    // shape they have. Read per call by write_structured/read_structured, so a
    // new pipeline is live without a restart — see src/core/pipeline.h.
    const std::string pipelines_dir = resolve_dir(funes::env("FUNES_PIPELINES_DIR"),
                                                  "pipelines");
    // The script library: one manifest + one executable file per script, and
    // the only place run_script will start anything from. Outside every
    // workspace on purpose — no agent-driven tool can read, edit or add a
    // script, so installing one is an admin editing the repo, the same act as
    // adding an agent. See src/core/script_library.h.
    const std::string scripts_dir = resolve_dir(funes::env("FUNES_SCRIPTS_DIR"),
                                                "scriptlib");

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

    // Same deal for the one-off maintenance commands (`funes cron-cleanup`).
    if (funes::is_maint_cli_command(argc, argv))
        return funes::run_maint_cli(argc, argv, db_path);

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
    // Relocate a pre-4.0 flat workspace into <root>/1/ before any tool looks
    // for a file in it.
    migrate_workspace_to_per_user(workspace_dir);

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

    // The agent loop lists an agent's granted scripts in its system prompt
    // from here, so it must be set before FunesApi copies `defaults`.
    defaults.scripts_dir = scripts_dir;

    // Every process Funes starts — a library script, a shell command, an MCP
    // stdio server — gets the filtered environment from
    // proc::child_environment(), never this process's own block. The config
    // loader above put funes.local into that block, so "inherit everything"
    // would hand the service token and every API key to any program a model
    // can start. The MCP client is vendored code that knows nothing about
    // Funes; it takes the filter as a hook (see mcp_stdio_client.h).
    mcp::stdio_client::set_environment_filter([](const json& env_vars) {
        std::vector<std::pair<std::string, std::string>> extra;
        for (const auto& [k, v] : env_vars.items())
            extra.emplace_back(k, v.is_string() ? v.get<std::string>() : v.dump());
        return funes::proc::child_environment(extra);
    });

    ToolRegistry tools;
    register_web_tools(tools);
    register_memory_tools(tools, memory);
    register_result_tools(tools, memory);
    register_context_tools(tools, memory, defaults);
    register_introspection_tools(tools);
    register_file_tools(tools, workspace_dir);
    register_shell_tool(tools, workspace_dir);
    register_script_tools(tools, workspace_dir, scripts_dir);
    register_harvest_tool(tools, memory, workspace_dir, publications_dir);
    register_publish_issue_tool(tools, workspace_dir, publishing_dir,
                                publications_dir);
    register_structured_tools(tools, workspace_dir, pipelines_dir);
    register_ranking_tools(tools);
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
    auto agent_roster = [&api](const std::string& exclude, const funes::Permissions& perms) {
        return api.agent_roster(exclude, perms);
    };
    api.set_agent_roster(agent_roster);
    defaults.agent_roster = agent_roster;

    // 5.0: the reply-language instruction the agent runtime appends. Resolved
    // per run rather than captured, so changing an account's locale takes
    // effect on its next message instead of at the next restart — and an
    // account that has since been deleted resolves to no locale at all, which
    // appends nothing.
    auto locale_for = [&users](int64_t user_id) -> std::string {
        auto u = users.find_by_id(user_id);
        return u ? u->locale : std::string();
    };
    api.set_user_locale(locale_for);
    defaults.user_locale = locale_for;

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
    // Resolved per firing, not captured at schedule time, so revoking an
    // account's access also stops its already-scheduled jobs. A job whose
    // owner has since been deleted resolves to no permissions at all rather
    // than to unrestricted.
    auto find_permissions_for_cron = [&users](int64_t user_id) {
        auto u = users.find_by_id(user_id);
        if (!u) {
            std::cerr << "[cron] job owner " << user_id
                      << " no longer exists; running with no permissions\n";
            return funes::Permissions::parse("{}", /*is_admin=*/false);
        }
        return funes::Permissions::parse(u->permissions, u->is_admin());
    };
    // A second install that opens a copy of the first's database inherits its
    // scheduled jobs and will fire them too — two newsletters, two WhatsApp
    // reminders to a real person. The jobs are data, so nothing about copying
    // a database says "but do not run these", and a parallel install is
    // exactly the situation where the copy is the point. Hence an explicit
    // switch, defaulting on so a normal install is unaffected.
    if (funes::env("FUNES_CRON_ENABLED", "1") == "1") {
        funes::cron::start_cron_runner(memory, tools, defaults, workspace_dir,
                                       find_agent_for_cron, find_permissions_for_cron,
                                       funes::env_int("FUNES_CRON_POLL_SECONDS", 30));
    } else {
        // Loud, not silent: a job that never fires is otherwise indis-
        // tinguishable from a job that fires and fails quietly.
        std::cerr << "[cron] disabled by FUNES_CRON_ENABLED=0 — "
                  << memory.list_cron_jobs(-1).size()
                  << " scheduled job(s) in this database will NOT run\n";
    }

    // Embed any memories that are missing vectors (e.g. stored while the
    // embedding endpoint was down) without blocking startup.
    //
    // Drained in a loop, not called once. backfill_embeddings() is capped per
    // call because it takes the write lock per row on a server that is already
    // answering requests; a single call therefore leaves everything past the
    // cap on keyword-only recall until the next restart. That went unnoticed
    // for as long as it did because only a migration drops every vector at
    // once — normally there are a handful to fill, never hundreds.
    //
    // A pass returning 0 is ambiguous: nothing left, or the embedding endpoint
    // is down (backfill_embeddings stops at the first failed embed rather than
    // burning through the whole backlog against a dead endpoint). Asking how
    // many are still missing separates the two, so the endpoint coming up
    // *after* Funes does — the ordinary case when both start at boot — gets
    // retried instead of waiting for a restart, and a real "all done" stops
    // the thread for good.
    std::thread([&memory] {
        constexpr auto kPassGap    = std::chrono::seconds(2);
        constexpr auto kRetryGap   = std::chrono::minutes(5);
        constexpr int  kMaxRetries = 12;   // ~1h of a down endpoint, then give up

        size_t total = 0;
        for (int retries = 0; ; ) {
            const size_t n = memory.backfill_embeddings();
            total += n;
            if (n > 0) {
                retries = 0;                  // progress resets the patience
                std::this_thread::sleep_for(kPassGap);
                continue;
            }
            const int64_t remaining = memory.count_missing_vectors();
            if (remaining == 0) break;
            if (++retries > kMaxRetries) {
                std::cerr << "[funes] giving up on " << remaining
                          << " memory embedding(s) — the embedding endpoint has been "
                             "unreachable for a while; they stay on keyword recall "
                             "until the next restart\n";
                break;
            }
            std::cerr << "[funes] " << remaining << " memory embedding(s) still missing; "
                         "embedding endpoint unreachable, retrying in "
                      << std::chrono::duration_cast<std::chrono::minutes>(kRetryGap).count()
                      << "m (" << retries << "/" << kMaxRetries << ")\n";
            std::this_thread::sleep_for(kRetryGap);
        }
        if (total > 0)
            std::cerr << "[funes] backfilled " << total << " memory embeddings\n";
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

    // Link backfill (5.0, see MemoryStore::backfill_links): propose links
    // between memories that are related without being duplicates. Deliberately
    // the slowest of the background passes — it is one model call per candidate
    // pair, and on a 271-memory pool the first full sweep is a few hours of
    // Qwen on the local GPU. Capped per run and resumable (every verdict is
    // recorded), so it drains over several nights instead of one long burst
    // competing with the chat traffic it shares a GPU with.
    //
    // Off by default: it costs real GPU time on a machine where the chat path
    // has to stay responsive, and an install that never turns it on keeps
    // exactly the 4.x recall it had.
    if (funes::env("FUNES_LINK_BACKFILL", "off") == "on") {
        MemoryStore::LinkBackfillOptions opt;
        opt.max_anchors = static_cast<size_t>(funes::env_int("FUNES_LINK_MAX_ANCHORS", 40));
        const int every_hours = std::max(1, funes::env_int("FUNES_LINK_HOURS", 12));

        std::thread([&memory, &defaults, opt, every_hours] {
            LLMClient llm(defaults.llm_url, defaults.llm_api_key,
                          defaults.llm_model, defaults.llm_provider);
            llm.set_max_tokens(8);   // one word; anything longer is not an answer
            for (;;) {
                std::this_thread::sleep_for(std::chrono::hours(every_hours));
                try {
                    auto report = memory.backfill_links(
                        [&llm](const std::string& a, const std::string& b) {
                            return judge_link(llm, a, b);
                        }, opt);
                    if (report.judged || report.linked)
                        std::cerr << "[funes] link backfill: " << report.anchors_seen
                                  << " anchor(s), judged " << report.judged
                                  << ", linked " << report.linked
                                  << ", skipped " << report.skipped << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "[funes] link backfill run failed: " << e.what() << "\n";
                }
            }
        }).detach();
    }

    // ── HTTP server ───────────────────────────────────────────────────────────
    httplib::Server srv;
    srv.set_read_timeout(60);
    srv.set_write_timeout(1200);   // SSE chat responses can be slow on local LLMs
    // httplib reads the whole body into memory *before* the pre-routing auth
    // gate runs, so without a cap an unauthenticated client could hand the
    // process a multi-gigabyte request. The largest legitimate body is an
    // upload-batch (50 MB, api.cpp); anything past that is refused at the
    // socket with a 413.
    srv.set_payload_max_length(64ull * 1024 * 1024);
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
