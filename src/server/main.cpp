// =============================================================================
// src/server/main.cpp — the funes binary
// =============================================================================
//
// One process serves everything: the web UI, the REST/SSE API, the agent
// runtime, and the memory engine. Start it, open http://localhost:8484,
// and talk to something that remembers you.

#include "funes_config.h"
#include "agent.h"
#include "mcp_pool.h"
#include "api.h"
#include "cron_runner.h"
#include "version.h"
#include "memory.h"
#include "permissions.h"
#include "tools.h"
#include "tools/cron_tool.h"
#include "tools/notify_tool.h"
#include "tools/http_tool_runtime.h"
#include "extension.h"
#include "tools/process_runner.h"
#include "mcp_stdio_client.h"
#include "maint_cli.h"
#include "local_socket.h"
#include "busy_gate.h"
#include "fact_extraction.h"
#include "host_collector.h"
#include "host_events.h"
#include "guardian.h"
#include "doctor.h"
#include "heal.h"
#include "incidents.h"
#include "probes.h"
#include "script_library.h"
#include "sandbox.h"
#include "staging.h"
#include "sd_notify.h"
#include "user_cli.h"
#include "users.h"
#include "httplib.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
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
// but broke a directory embedded into an argv for a subprocess: that child
// is spawned with cwd = the workspace directory, not this process's cwd, so a
// relative path resolved against the wrong directory and the interpreter
// could not find the script. Found on 2026-07-31 in a live run that got all
// the way to `exit 2: No such file or directory`.
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

// Where runtime state lives unless FUNES_DB / FUNES_WORKSPACE_DIR say
// otherwise: data/ beside bin/, inside the checkout, so one directory holds
// the code, its configuration and everything it writes (5.2 — before that the
// default was ~/.funes, and production had drifted into ~/.funes-v4).
// Anchored on the binary rather than the cwd, so a CLI subcommand run from
// somewhere else still opens the same database the service does.
static std::string data_dir() {
    const std::string configured = funes::env("FUNES_DATA_DIR");
    if (!configured.empty()) return fs::absolute(configured).string();
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return (fs::path(buf).parent_path().parent_path() / "data").string();
    }
    return fs::absolute("data").string();
}

// A colon-separated directory list (FUNES_AGENTS_DIR, FUNES_SCRIPTS_DIR):
// each entry made absolute, empty entries dropped, the default used only when
// nothing is configured at all. Order is kept — later entries shadow earlier
// ones by name in both places that read these.
static std::string resolve_dir_list(const std::string& configured, const std::string& relative) {
    if (configured.empty()) return resolve_dir("", relative);
    std::string out;
    size_t pos = 0;
    while (pos <= configured.size()) {
        size_t end = configured.find(':', pos);
        if (end == std::string::npos) end = configured.size();
        const std::string one = configured.substr(pos, end - pos);
        if (!one.empty()) out += (out.empty() ? "" : ":") + resolve_dir(one, relative);
        pos = end + 1;
    }
    return out;
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

namespace {

void print_usage(std::ostream& out) {
    out << "funes " << funes::version() << "\n\n"
           "  funes                       start the server (see config/funes.conf)\n"
           "  funes --version | version   print the version and exit\n"
           "  funes --help | help         this text\n\n"
           "  funes ask [question]        ask the running server over funes.sock; pipe input in\n\n"
           "Accounts (run against $FUNES_DB, no server, no LLM):\n"
           "  useradd  userdel  userlist  passwd  perms  locale  jid-map  jid-unmap\n"
           "  uid-map  uid-unmap   (who a local connection to funes.sock is)\n\n"
           "Maintenance:\n"
           "  cron-cleanup                remove pre-4.1 scheduled-run leftovers\n"
           "  cron-schedule <id> \"<expr>\" change when a scheduled job fires\n"
           "  memory-migrate              queue stored exchange logs for fact extraction\n"
           "  script-run                  run a library script as run_script would (sandboxed)\n"
           "  script-learn                watch a script run, propose its access: block\n"
           "  mcp-probe                   start an agent's MCP stdio servers sandboxed, list tools (--learn)\n"
           "  machine-probe               what the machine collector would read here (read-only)\n";
}

} // namespace

int main(int argc, char** argv) {
    // Argument handling comes before load_config() and before anything derives
    // a database path, because the failure this prevents is exactly that: an
    // unrecognised argument used to fall through to "start the server", so
    // `funes --version` asked a harmless question and got a live server on the
    // default port plus a fresh database. Nothing below
    // this block runs unless argv names something real.
    if (argc > 1 && argv[1] != nullptr) {
        const std::string arg = argv[1];
        // The child-process trampoline (process_runner.h): not a command a
        // person runs, and nothing — config, database, stdio — may be touched
        // on the way to exec'ing the real program.
        if (arg == "_exec") return funes::proc::trampoline_main(argc, argv);
        // A client of a running server's funes.sock (local_socket.h): no config,
        // no database — the server decides who the caller is.
        if (arg == "ask") return funes::ask_main(argc, argv);
        if (arg == "--version" || arg == "-V" || arg == "version") {
            std::cout << "funes " << funes::version() << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h" || arg == "help") {
            print_usage(std::cout);
            return 0;
        }
        if (!funes::is_user_cli_command(argc, argv) &&
            !funes::is_maint_cli_command(argc, argv)) {
            std::cerr << "funes: unknown command '" << arg << "'\n\n";
            print_usage(std::cerr);
            return 2;
        }
    }

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
    {
        // "1" was the pre-7.2 spelling of "on" and meant the exchange log;
        // it now means the default, extraction. Only "log" keeps the old rows.
        const std::string mode = funes::env("FUNES_AUTO_MEMORY", "extract");
        defaults.auto_memory = mode == "0" || mode == "off" ? AgentDefaults::AutoMemory::Off
                             : mode == "log"                ? AgentDefaults::AutoMemory::Log
                                                            : AgentDefaults::AutoMemory::Extract;
    }

    const std::string host          = funes::env("FUNES_HOST", "127.0.0.1");
    const int         port          = funes::env_int("FUNES_PORT", 8484);
    const std::string default_agent = funes::env("FUNES_DEFAULT_AGENT", "funes");
    // Colon-separated: the repo's agents/ plus an extension's, say. The first
    // entry is where create_agent writes; every entry is loaded, later ones
    // shadowing earlier ones on a name collision (api.cpp::load_agents).
    const std::string agents_dir = resolve_dir_list(funes::env("FUNES_AGENTS_DIR"), "agents");
    const std::string ui_dir        = resolve_dir(funes::env("FUNES_UI_DIR"), "ui");
    const std::string generated_tools_dir = resolve_dir(
        funes::env("FUNES_GENERATED_TOOLS_DIR"), "src/core/tools/generated");
    // The script library: one manifest + one executable file per script, and
    // the only place run_script will start anything from. Outside every
    // workspace on purpose — no agent-driven tool can read, edit or add a
    // script, so installing one is an admin editing the repo, the same act as
    // adding an agent. Colon-separated like FUNES_AGENTS_DIR, so an
    // extension's scriptlib/ loads beside this one. See src/core/script_library.h.
    const std::string scripts_dir = resolve_dir_list(funes::env("FUNES_SCRIPTS_DIR"),
                                                     "scriptlib");

    std::string db_path = funes::env("FUNES_DB");
    if (db_path.empty()) {
        std::error_code ec;
        fs::create_directories(data_dir(), ec);
        db_path = data_dir() + "/memory.db";
    } else {
        db_path = fs::absolute(db_path).string();
    }

    // Every process Funes starts — a library script, a shell command, an MCP
    // stdio server — gets the filtered environment from
    // proc::child_environment(), never this process's own block. The config
    // loader above put funes.local into that block, so "inherit everything"
    // would hand the service token and every API key to any program a model
    // can start. The MCP client is vendored code that knows nothing about
    // Funes; it takes the filter as a hook (see mcp_stdio_client.h). Set
    // before the CLI dispatch below: `funes mcp-probe` starts servers too.
    mcp::stdio_client::set_environment_filter([](const json& env_vars) {
        std::vector<std::pair<std::string, std::string>> extra;
        for (const auto& [k, v] : env_vars.items())
            extra.emplace_back(k, v.is_string() ? v.get<std::string>() : v.dump());
        return funes::proc::child_environment(extra);
    });

    // Agent YAML names checkout paths as ${FUNES_ROOT}; src/julio/register.cpp
    // publishes it for the server, but the CLI dispatch below runs first.
    if (funes::env("FUNES_ROOT").empty()) ::setenv("FUNES_ROOT", FUNES_ROOT, 0);

    // Account management (`funes useradd` …) runs against the same database
    // and exits without starting the server. Dispatched here, after the config
    // and the database path are known but before anything opens a socket or
    // queries the LLM — a CLI invocation must not need a reachable model.
    if (funes::is_user_cli_command(argc, argv))
        return funes::run_user_cli(argc, argv, db_path);

    // Same deal for the one-off maintenance commands (`funes cron-cleanup`).
    if (funes::is_maint_cli_command(argc, argv)) {
        const std::string ws = funes::env("FUNES_WORKSPACE_DIR");
        return funes::run_maint_cli(argc, argv, db_path, scripts_dir,
                                    ws.empty() ? data_dir() + "/workspace" : fs::absolute(ws).string(),
                                    agents_dir);
    }

    // A cgroup per child process (cgroup.h), before anything can start one.
    const funes::cgroup::Status cgroups = funes::cgroup::init();
    if (cgroups.enabled || funes::sandbox::enabled()) funes::proc::set_trampoline("/proc/self/exe");

    // read_file/write_file/execute_shell are confined to this directory.
    std::string workspace_dir = funes::env("FUNES_WORKSPACE_DIR");
    workspace_dir = workspace_dir.empty() ? data_dir() + "/workspace"
                                          : fs::absolute(workspace_dir).string();
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
    // What the effects gate holds for the person to approve (effects.h).
    funes::ProposalStore proposals(db_path);

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
    defaults.proposals = &proposals;

    // Staging (staging.h): a tainted run's host script runs against a copy,
    // kept beside memory.db for as long as its proposal waits. Probed once,
    // through the trampoline it will really use.
    funes::staging::set_root(funes::env("FUNES_STAGING_DIR",
                                        (fs::path(db_path).parent_path() / "staging").string()));
    std::string staging_reason;
    funes::staging::probe(staging_reason);
    funes::staging::prune(proposals.live_stages(), 0);

    // The machine as a memory owner (OS plan phase 4, host_collector.h): a
    // `system` account nobody can log in as owns the pool; the event ring
    // lives beside it in memory.db. FUNES_MACHINE_MEMORY=off skips both. The
    // account is made on first use, never before a person exists (users.h),
    // and never over a person already called `machine`.
    std::unique_ptr<funes::host::EventStore> host_events;
    if (funes::env("FUNES_MACHINE_MEMORY", "on") != "off")
        host_events = std::make_unique<funes::host::EventStore>(db_path);
    // Self-healing H0 (probes.h, incidents.h): health checks on a timer,
    // incidents, a WhatsApp notice to the admins. FUNES_PROBES=off skips it.
    std::unique_ptr<funes::incidents::IncidentStore> incidents;
    if (funes::env("FUNES_PROBES", "on") != "off")
        incidents = std::make_unique<funes::incidents::IncidentStore>(db_path);
    std::atomic<int64_t> machine_uid{0};
    auto machine_account = [&users, &machine_uid]() -> int64_t {
        if (const int64_t id = machine_uid.load()) return id;
        static std::atomic<bool> warned{false};
        const int64_t id = users.ensure_system_account("machine");
        if (id > 0) machine_uid = id;
        else if (users.find_by_username("machine") && !warned.exchange(true))
            std::cerr << "[machine] the username 'machine' belongs to a person on this "
                         "install; machine events are recorded but not remembered\n";
        return id;
    };

    // The guardian (self-healing H1, guardian.h): scripts/funes_guardian.py in
    // this checkout, unless FUNES_GUARDIAN=off. Absent on a checkout without
    // it, and then create_agent writes unguarded, as before 7.19.
    std::string guardian_script;
    if (funes::env("FUNES_GUARDIAN", "on") != "off") {
        const std::string scripts = resolve_dir("", "scripts");
        if (fs::exists(scripts + "/funes_guardian.py")) {
            guardian_script = scripts + "/funes_guardian.py";
            funes::guardian::configure(guardian_script, fs::path(scripts).parent_path().string());
        }
    }

    ToolRegistry tools;
    register_web_tools(tools);
    register_memory_tools(tools, memory);
    register_host_tools(tools, host_events.get(), memory, machine_account);
    register_incident_tools(tools, incidents.get());
    register_result_tools(tools, memory);
    register_context_tools(tools, memory, defaults);
    register_classify_decision_tool(tools, defaults);
    register_introspection_tools(tools);
    register_file_tools(tools, workspace_dir);
    register_shell_tool(tools, workspace_dir);
    register_script_tools(tools, workspace_dir, scripts_dir);
    // notify_user: Funes's own WhatsApp number (whatsapp-bridge-funes), to the
    // jid mapped to the run's account. The same WHATSAPP_BRIDGE_URL the
    // autoresponder replies through — never the personal-number bridge.
    {
        const std::string bridge = funes::env("WHATSAPP_BRIDGE_URL");
        register_notify_tool(tools, {
            [&users](int64_t uid) { return users.jid_for_user(uid); },
            [bridge](const std::string& jid, const std::string& text) {
                return send_via_whatsapp_bridge(bridge, jid, text);
            }});
    }
    funes::tools::register_all_generated_tools(tools);
    register_tool_builder(tools, generated_tools_dir);
    // Self-registering tool packs (src/julio; see src/core/extension.h).
    // Nothing here knows what they are; each queued itself from a static
    // initializer.
    {
        const size_t n = funes::ext::register_all_extensions({tools, memory, workspace_dir});
        if (n > 0) std::cerr << "[funes] " << n << " extension(s) registered tools\n";
    }

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
    // create_agent writes into the first agents directory only.
    register_agent_builder(tools, agents_dir.substr(0, agents_dir.find(':')),
                           [&api] { api.load_agents(); });

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
        opt.archive_uncited_after_days = funes::env_int("FUNES_ARCHIVE_UNCITED_DAYS", 90);
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
                            funes::load::wait_until_quiet();
                            return merge_memories(llm, texts);
                        }, opt);
                    if (report.merged || report.pruned || report.clusters_seen || report.archived)
                        std::cerr << "[funes] consolidation: " << report.clusters_seen
                                  << " cluster(s), merged " << report.merged
                                  << ", kept " << report.kept
                                  << ", pruned " << report.pruned
                                  << ", archived uncited " << report.archived << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "[funes] consolidation run failed: " << e.what() << "\n";
                }
            }
        }).detach();
    }

    // Fact extraction (7.2, fact_extraction.h): turns queued exchanges into
    // the facts they established. Runs unless FUNES_AUTO_MEMORY is off: in
    // extract mode (the default) it handles new exchanges; in log mode
    // nothing new is queued and it handles only what `funes memory-migrate`
    // queued (7.3). Polls rather than being woken by the chat path, and only
    // takes exchanges a couple of minutes old, so a conversation in progress
    // is not competing with its own memory for the GPU; each call also waits
    // for busy_gate like the passes below.
    if (defaults.auto_memory != AgentDefaults::AutoMemory::Off) {
        funes::FactExtractionOptions opt;
        opt.max_items       = static_cast<size_t>(funes::env_int("FUNES_EXTRACT_MAX_ITEMS", 20));
        opt.min_age_seconds = funes::env_int("FUNES_EXTRACT_MIN_AGE", 120);
        const int poll = std::max(10, funes::env_int("FUNES_EXTRACT_POLL", 60));

        std::thread([&memory, &defaults, opt, poll] {
            LLMClient llm(defaults.llm_url, defaults.llm_api_key,
                          defaults.llm_model, defaults.llm_provider);
            llm.set_max_tokens(512);
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(poll));
                try {
                    const auto report = funes::run_fact_extraction(memory,
                        [&llm](const std::vector<ChatMessage>& msgs) {
                            funes::load::wait_until_quiet();
                            return llm.complete(msgs).content;
                        }, opt);
                    if (report.seen)
                        std::cerr << "[funes] fact extraction: " << report.seen
                                  << " exchange(s), " << report.facts << " fact(s), "
                                  << report.replaced << " superseded, "
                                  << report.archived << " log(s) archived, "
                                  << report.failed << " failed\n";
                } catch (const std::exception& e) {
                    std::cerr << "[funes] fact extraction run failed: " << e.what() << "\n";
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
    // On by default since 7.0. It was off because it cost GPU time on a
    // machine where the chat path has to stay responsive; each judgment now
    // waits for no agent run to be in flight and for CPU/IO pressure to be low
    // (busy_gate.h), so a person waits behind at most one one-word answer.
    // FUNES_LINK_BACKFILL=off still turns it off.
    if (funes::env("FUNES_LINK_BACKFILL", "on") == "on") {
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
                            funes::load::wait_until_quiet();
                            return judge_link(llm, a, b);
                        }, opt);
                    if (report.judged || report.linked)
                        std::cerr << "[funes] link backfill: " << report.anchors_seen
                                  << " anchor(s), judged " << report.judged
                                  << ", linked " << report.linked
                                  << ", superseded " << report.superseded
                                  << ", skipped " << report.skipped << "\n";
                } catch (const std::exception& e) {
                    std::cerr << "[funes] link backfill run failed: " << e.what() << "\n";
                }
            }
        }).detach();
    }

    // Machine collector (OS plan phase 4). A tick a minute: the user journal
    // (filtered at the source), dpkg.log, boot/kernel/deploys, the OOM
    // counter, the llama-server pid files, watched files, and every five
    // minutes PSI, temperatures, VRAM and disks. Changes and failures become
    // memories in the machine's pool — embedded by the otherwise idle
    // embedding server, each write behind the same busy gate as the link
    // backfill so a person's chat never waits on the machine.
    std::unique_ptr<funes::host::Collector> collector;
    if (host_events) {
        funes::host::CollectorOptions o;
        o.funes_version      = funes::version();
        o.disks              = {"/", data_dir()};
        o.promotions_per_day = std::max(1, funes::env_int("FUNES_MACHINE_PROMOTIONS_PER_DAY", 200));
        o.retention_days     = std::max(1, funes::env_int("FUNES_MACHINE_EVENT_DAYS", 30));
        o.llama_pid_dir      = funes::env("FUNES_MACHINE_LLAMA_PIDS", "/tmp/llama_pids");
        const std::string home = funes::env("HOME");
        std::string watch = funes::env("FUNES_MACHINE_WATCH");
        if (watch.empty() && !home.empty())
            watch = home + "/.config/systemd/user:" + home + "/launch_llama.sh:" +
                    fs::path(db_path).parent_path().parent_path().string() + "/config";
        std::stringstream ws(watch);
        for (std::string p; std::getline(ws, p, ':');) if (!p.empty()) o.watch.push_back(p);

        // journalctl as any other child: its own cgroup, and a sandbox that
        // reads the journal and nothing else of note.
        funes::sandbox::Policy jp = funes::sandbox::base();
        jp.read.push_back("/var/log/journal");
        auto run = [jp](const std::vector<std::string>& argv) -> std::optional<std::string> {
            const auto r = funes::proc::run_argv(argv, "/", 30, 8u << 20, {}, true,
                                                 std::nullopt, jp);
            if (r.timed_out || r.exit_code != 0) {
                std::cerr << "[machine] journalctl failed (exit " << r.exit_code << "): "
                          << r.stderr_output.substr(0, 200) << "\n";
                return std::nullopt;
            }
            return r.output;
        };
        auto remember = [&memory, machine_account](const std::string& text, bool tainted,
                                                   const std::string& origin) -> int64_t {
            const int64_t uid = machine_account();
            if (uid <= 0) return 0;
            funes::load::wait_until_quiet();
            return memory.remember(uid, "machine", text, "machine",
                                   funes::Provenance{origin, tainted});
        };
        auto link = [&memory, machine_account](int64_t from, int64_t to) {
            if (const int64_t uid = machine_account())
                memory.link_memories(uid, from, to, "preceded_by", 0.6);
        };
        collector = std::make_unique<funes::host::Collector>(*host_events, o, run, remember, link);

        funes::host::Collector* c = collector.get();
        funes::proc::set_usage_observer([c](const std::vector<std::string>& argv,
                                            const funes::cgroup::Usage& u) {
            c->record_run(argv, u, static_cast<int64_t>(std::time(nullptr)));
        });
        const int poll = std::max(15, funes::env_int("FUNES_MACHINE_POLL", 60));
        std::thread([c, poll] {
            for (;;) {
                try {
                    const auto r = c->tick(static_cast<int64_t>(std::time(nullptr)));
                    if (r.promoted)
                        std::cerr << "[machine] " << r.events << " event(s), " << r.promoted
                                  << " remembered\n";
                } catch (const std::exception& e) {
                    std::cerr << "[machine] tick failed: " << e.what() << "\n";
                }
                std::this_thread::sleep_for(std::chrono::seconds(poll));
            }
        }).detach();
    }

    // ── Self-healing probes (H0) ──────────────────────────────────────────────
    // Script probes are library scripts with a `probe:` block, run in the
    // sandbox run_script would give them (their `access:`), from a working
    // directory of their own; the built-in one reads the cron jobs. Notices go
    // to every admin with a mapped WhatsApp number, from Funes's own number,
    // in that admin's language.
    if (incidents) {
        funes::probes::Options po;
        po.list_scripts = [scripts_dir]() {
            std::vector<funes::probes::ScriptProbe> out;
            for (const auto& name : funes::list_script_names(scripts_dir)) {
                funes::ScriptSpec spec;
                if (funes::load_script(scripts_dir, name, spec).empty() && spec.probe_every > 0)
                    out.push_back({name, spec.probe_every, spec.probe_fail_after});
            }
            return out;
        };
        const fs::path probe_dir = fs::path(workspace_dir) / "_probes";
        po.run_script = [scripts_dir, probe_dir](const std::string& name, json& out) -> std::string {
            funes::ScriptSpec spec;
            if (std::string err = funes::load_script(scripts_dir, name, spec); !err.empty()) return err;
            std::vector<std::string> argv;
            if (std::string err = funes::build_argv(spec, json::object(), argv); !err.empty()) return err;
            std::error_code ec;
            fs::create_directories(probe_dir, ec);
            const auto r = funes::proc::run_argv(argv, probe_dir.string(), spec.timeout_seconds,
                                                 1u << 20, spec.env, true, spec.limits,
                                                 funes::script_policy(spec, probe_dir));
            if (r.timed_out) return "timed out after " + std::to_string(spec.timeout_seconds) + "s";
            if (r.exit_code != 0)
                return "exit " + std::to_string(r.exit_code) + ": " + r.stderr_output.substr(0, 400);
            return funes::validate_output(spec, r.output, out);
        };
        // fail_after 2: a job that runs every minute (the TV's) fails now and
        // then when the TV does not answer; two checks in a row is a job that
        // is broken, not one that had a bad minute.
        po.builtins.push_back({"jobs", 120, 2, [&memory]() {
            std::vector<funes::incidents::CheckResult> checks;
            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            for (const auto& j : memory.list_cron_jobs(-1)) {
                if (j.last_run_at == 0 || j.running) continue;
                // A one-shot job never runs again: its failure is news for a
                // day, then it stops being reported (and its incident closes).
                if (j.done_at && now - j.done_at > 86400) continue;
                const bool ok = j.last_status == "ok";
                checks.push_back({"jobs:" + std::to_string(j.id), ok, "warning",
                                  "Scheduled job #" + std::to_string(j.id) + " '" + j.name + "' " +
                                      (ok ? "ran fine" : "failed its last run"),
                                  ok ? "" : j.last_output});
            }
            return checks;
        }});
        po.notify = [&users](const funes::incidents::Incident& i) {
            const std::string bridge = funes::env("WHATSAPP_BRIDGE_URL");
            bool reached = false, anyone = false;
            for (const auto& u : users.list_users()) {
                if (u.role != UserStore::ROLE_ADMIN) continue;
                const std::string jid = users.jid_for_user(u.id);
                if (jid.empty()) continue;
                anyone = true;
                const std::string err = send_via_whatsapp_bridge(
                    bridge, jid, funes::probes::notice_text(i, u.locale));
                if (err.empty()) reached = true;
                else std::cerr << "[probe] notice to " << u.username << " failed: " << err << "\n";
            }
            if (!anyone) {
                // Nobody to tell is not a reason to keep it pending for ever:
                // the incident stays in the table and the `incidents` tool.
                std::cerr << "[probe] no admin has a WhatsApp number mapped (funes jid-map); "
                             "incident #" << i.id << " not sent\n";
                return true;
            }
            return reached;
        };
        // H2 (heal.h): config/heal.yaml says what to do when an incident
        // opens. A proposal or an action belongs to the first admin, runs with
        // the rule's agent's grants, and is recorded like any approved call.
        const std::string heal_path = funes::env("FUNES_HEAL_POLICY", resolve_dir("", "config") + "/heal.yaml");
        auto first_admin = [&users]() -> std::optional<UserStore::User> {
            for (const auto& u : users.list_users())
                if (u.role == UserStore::ROLE_ADMIN) return u;
            return std::nullopt;
        };
        funes::heal::Options ho;
        ho.now = [] { return static_cast<int64_t>(std::time(nullptr)); };
        ho.locale = first_admin() ? first_admin()->locale : "en";
        auto make_proposal = [&proposals, first_admin](const funes::heal::Rule& r,
                                                       const funes::incidents::Incident& i) -> int64_t {
            const auto admin = first_admin();
            if (!admin) return 0;
            return proposals.create(admin->id, r.agent, r.tool, r.args, "heal-" + std::to_string(i.id),
                                    "incident #" + std::to_string(i.id) + " (" + i.key + ")",
                                    funes::effect_name(funes::builtin_effect(r.tool)));
        };
        ho.propose = make_proposal;
        ho.act = [&proposals, &api, &tools, &memory, &defaults, first_admin, make_proposal](
                     const funes::heal::Rule& r, const funes::incidents::Incident& i) -> std::pair<bool, std::string> {
            const auto admin = first_admin();
            const int64_t id = make_proposal(r, i);
            if (!admin || !id) return {false, "no admin to act as"};
            if (proposals.decide(admin->id, id, true) != funes::ProposalStore::Decision::Ok)
                return {false, "proposal #" + std::to_string(id) + " could not be approved"};
            const auto p = proposals.get(admin->id, id);
            const AgentConfig cfg = api.find_agent(r.agent);
            ToolResult res;
            if (!p || cfg.name.empty()) res = {"the agent '" + r.agent + "' does not exist", true};
            else {
                try {
                    FunesAgent agent(cfg, tools, memory, defaults);
                    res = agent.execute_proposal(*p, funes::Permissions::parse(admin->permissions, true));
                } catch (const std::exception& e) {
                    res = {e.what(), true};
                }
            }
            proposals.finish(admin->id, id, !res.error, res.text);
            return {!res.error, res.text};
        };
        auto load_policy = [heal_path]() {
            funes::heal::Policy p;
            std::ifstream f(heal_path);
            if (!f) return p;
            std::stringstream s;
            s << f.rdbuf();
            if (const std::string err = funes::heal::parse_policy(s.str(), p); !err.empty())
                std::cerr << "[heal] " << heal_path << " ignored: " << err << "\n";
            return p;
        };
        // H3 (doctor.h): an incident no rule covers goes to the doctor, on its
        // own thread — a model call waits for the GPU to be free of chat, and
        // must not hold up the probes. What it picks is proposed, never run.
        struct DoctorJob { funes::incidents::Incident incident; std::vector<funes::doctor::Runbook> runbooks; };
        auto doctor_q = std::make_shared<std::pair<std::mutex, std::vector<DoctorJob>>>();
        auto doctor_cv = std::make_shared<std::condition_variable>();
        const bool doctor_on = funes::env("FUNES_DOCTOR", "on") != "off";
        // Shared, not a local: the callback outlives this block.
        auto healer_ref = std::make_shared<std::weak_ptr<funes::heal::Healer>>();
        ho.unmatched = [doctor_q, doctor_cv, doctor_on, healer_ref](const funes::incidents::Incident& i) {
            const auto h = healer_ref->lock();
            if (!doctor_on || !h) return;
            {
                std::lock_guard<std::mutex> lock(doctor_q->first);
                doctor_q->second.push_back({i, h->runbooks()});
            }
            doctor_cv->notify_one();
        };
        auto healer = std::make_shared<funes::heal::Healer>(load_policy(), std::move(ho));
        *healer_ref = healer;
        if (doctor_on) {
            const int per_day = std::max(1, funes::env_int("FUNES_DOCTOR_PER_DAY", 10));
            std::thread([doctor_q, doctor_cv, per_day, first_admin, &defaults, &tools, &proposals,
                         &users, inc = incidents.get()] {
                LLMClient llm(defaults.llm_url, defaults.llm_api_key, defaults.llm_model, defaults.llm_provider);
                llm.set_max_tokens(400);
                std::vector<int64_t> runs;
                for (;;) {
                    DoctorJob job;
                    {
                        std::unique_lock<std::mutex> lock(doctor_q->first);
                        doctor_cv->wait(lock, [&] { return !doctor_q->second.empty(); });
                        job = doctor_q->second.front();
                        doctor_q->second.erase(doctor_q->second.begin());
                    }
                    const int64_t now = static_cast<int64_t>(std::time(nullptr));
                    runs.erase(std::remove_if(runs.begin(), runs.end(),
                                              [now](int64_t t) { return now - t >= 86400; }), runs.end());
                    const auto admin = first_admin();
                    if (!admin || static_cast<int>(runs.size()) >= per_day) {
                        std::cerr << "[doctor] #" << job.incident.id << " skipped ("
                                  << (admin ? "daily limit" : "no admin") << ")\n";
                        continue;
                    }
                    runs.push_back(now);
                    funes::doctor::Case c{job.incident.key, job.incident.summary, job.incident.evidence, ""};
                    ToolContext ctx{"doctor", "doctor-" + std::to_string(job.incident.id)};
                    ctx.user_id = admin->id;
                    const auto changes = tools.call("host_changes", {{"since_hours", 6}}, ctx);
                    if (!changes.error) c.recent_changes = changes.text;
                    std::string refusal;
                    const auto v = funes::doctor::diagnose([&llm](const std::vector<ChatMessage>& m) {
                        funes::load::wait_until_quiet();
                        return llm.complete(m).content;
                    }, c, job.runbooks, &refusal);
                    const funes::doctor::Runbook* chosen = nullptr;
                    for (const auto& r : job.runbooks) if (r.id == v.choice) chosen = &r;
                    int64_t proposal = 0;
                    if (chosen)
                        proposal = proposals.create(admin->id, chosen->agent, chosen->tool, chosen->args,
                                                    "doctor-" + std::to_string(job.incident.id),
                                                    "doctor, incident #" + std::to_string(job.incident.id) +
                                                        " (" + job.incident.key + ")",
                                                    funes::effect_name(funes::builtin_effect(chosen->tool)));
                    std::cerr << "[doctor] #" << job.incident.id << " " << job.incident.key << " → " << v.choice
                              << (refusal.empty() ? "" : " (answer refused: " + refusal + ")")
                              << (proposal ? ", proposal #" + std::to_string(proposal) : "") << "\n";
                    const std::string bridge = funes::env("WHATSAPP_BRIDGE_URL");
                    for (const auto& u : users.list_users()) {
                        if (u.role != UserStore::ROLE_ADMIN) continue;
                        const std::string jid = users.jid_for_user(u.id);
                        if (jid.empty()) continue;
                        const std::string err = send_via_whatsapp_bridge(
                            bridge, jid, funes::doctor::verdict_text(c, v, chosen, proposal, u.locale));
                        if (!err.empty()) std::cerr << "[doctor] notice to " << u.username << " failed: " << err << "\n";
                    }
                    if (inc) {
                        const auto cur = inc->get(job.incident.id);
                        inc->set_action(job.incident.id, (cur ? cur->action : "") + "\n\n" +
                                        funes::doctor::verdict_text(c, v, chosen, proposal, admin->locale));
                    }
                }
            }).detach();
        }
        auto policy_mtime = std::make_shared<fs::file_time_type>();
        po.on_open = [healer, heal_path, policy_mtime, load_policy](const funes::incidents::Incident& i) {
            std::error_code ec;
            const auto mtime = fs::last_write_time(heal_path, ec);
            if (!ec && mtime != *policy_mtime) {
                *policy_mtime = mtime;
                healer->set_policy(load_policy());
            }
            return healer->on_open(i);
        };
        po.quiet_hours = funes::env("FUNES_INCIDENT_QUIET_HOURS", "23-8");
        po.local_hour = [](int64_t now) {
            std::time_t t = static_cast<std::time_t>(now);
            std::tm tm{};
            localtime_r(&t, &tm);
            return tm.tm_hour;
        };
        auto runner = std::make_shared<funes::probes::Runner>(*incidents, std::move(po));
        const int delay = std::max(0, funes::env_int("FUNES_PROBES_DELAY", 90));
        std::thread([runner, delay] {
            // After a boot the model may still be loading: give it a moment
            // before the first round, so the first notice is not a false one.
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            for (;;) {
                try {
                    const auto r = runner->tick(static_cast<int64_t>(std::time(nullptr)));
                    if (r.opened || r.resolved || r.noticed)
                        std::cerr << "[probe] " << r.ran << " ran, " << r.opened << " opened, "
                                  << r.resolved << " resolved, " << r.noticed << " notice(s) sent\n";
                } catch (const std::exception& e) {
                    std::cerr << "[probe] tick failed: " << e.what() << "\n";
                }
                std::this_thread::sleep_for(std::chrono::seconds(60));
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

    // funes.sock (OS plan phase 3, local_socket.h): the kernel names the
    // caller's uid; uid_users names the account. FUNES_SOCKET=off skips it.
    const std::string sock_path = funes::local_socket_path();
    const bool sock_ok = !sock_path.empty() && funes::start_local_socket(sock_path, api, users);

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
              << "  Children:  " << (cgroups.enabled
                     ? "a cgroup each in " + cgroups.dir + " (" + cgroups.controllers + ")"
                     : "no cgroup — " + cgroups.reason) << "\n"
              << "  Sandbox:   " << (funes::sandbox::enabled()
                     ? "Landlock ABI " + std::to_string(funes::sandbox::abi()) + " + seccomp"
                     : funes::sandbox::abi() > 0 ? std::string("off (FUNES_SANDBOX=0)")
                                                 : std::string("unavailable — no Landlock in this kernel"))
              << "\n"
              << "  Staging:   " << (funes::staging::available()
                     ? "host scripts in tainted runs run against an overlay in " + funes::staging::root()
                     : "off (" + staging_reason + ") — their calls are proposed unrun") << "\n"
              << "  Socket:    " << (sock_ok ? sock_path + " (uid-mapped accounts; `funes ask`)"
                                             : std::string("off")) << "\n"
              << "  Machine:   " << (!collector ? std::string("memory off")
                                     : machine_account() > 0
                                         ? "memory on (account #" + std::to_string(machine_account()) + ")"
                                         : std::string("events on; memory starts once a person has an account"))
              << "\n"
              << "  Guardian:  " << (guardian_script.empty() ? std::string("off — create_agent writes unguarded")
                                                            : guardian_script + " (\"deshacer N\" undoes change N)")
              << "\n"
              << "  Probes:    " << (incidents ? "on (incidents to admins on WhatsApp, quiet " +
                                                     funes::env("FUNES_INCIDENT_QUIET_HOURS", "23-8") + ")"
                                                : std::string("off"))
              << "\n"
              << "\n" << std::flush;

    signal(SIGPIPE, SIG_IGN);  // dropped SSE clients must not kill the process

    if (!srv.bind_to_port(host, port)) {
        std::cerr << "[funes] FATAL: cannot listen on " << host << ":" << port
                  << " (port in use?)\n";
        return 1;
    }

    // systemd (Type=notify): READY=1 once requests are actually being
    // accepted, then WATCHDOG=1 only while a request to ourselves is answered.
    // Any status counts as an answer — the probe asks "is a worker free to
    // pick this up", not "is the LLM up"; a slow model is not a dead server,
    // but every worker stuck behind one is. No-ops outside systemd.
    const std::string probe_host = (host == "0.0.0.0" || host == "::") ? "127.0.0.1" : host;
    std::thread([&srv] {
        for (int i = 0; i < 500 && !srv.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        funes::sd::notify("READY=1");
    }).detach();
    funes::sd::Watchdog watchdog(funes::sd::watchdog_interval(), [probe_host, port] {
        httplib::Client cli(probe_host, port);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);
        return static_cast<bool>(cli.Get("/api/auth/status"));
    });

    const bool ok = srv.listen_after_bind();
    funes::sd::notify("STOPPING=1");
    funes::McpPool::instance().clear();   // stop kept-alive MCP servers (mcp_pool.h)
    return ok ? 0 : 1;
}
