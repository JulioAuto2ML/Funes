// =============================================================================
// src/server/maint_cli.cpp — database maintenance subcommands
// =============================================================================

#include "maint_cli.h"
#include "../core/cron_schedule.h"
#include "../core/sandbox.h"
#include "../core/script_library.h"
#include "../core/tools.h"
#include "../core/tools/process_runner.h"
#include "../core/memory.h"
#include "../core/users.h"
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace funes {
namespace {

const char* const COMMANDS[] = {"cron-cleanup", "cron-schedule", "memory-migrate", "script-run"};

void usage() {
    std::cerr <<
        "Usage:\n"
        "  funes cron-cleanup [--apply] [--drop-sessions] [--user <username>]\n"
        "\n"
        "Removes the auto-memories scheduled jobs used to leave behind —\n"
        "the ones phrased as though the person had typed the job's task.\n"
        "Reports without deleting unless --apply is given.\n"
        "\n"
        "  --drop-sessions  also delete the per-run transcripts. Off by\n"
        "                   default: they are already hidden from the\n"
        "                   conversation list, and they are the only record\n"
        "                   of what a run older than the last one did.\n"
        "\n"
        "  funes cron-schedule <job-id> \"<m h dom mon dow>\"\n"
        "\n"
        "Changes when a scheduled job fires, and moves its next run to match\n"
        "(any user's job — this is the admin path). Safe while the server runs:\n"
        "the runner reads the table on every tick.\n"
        "\n"
        "  funes memory-migrate [--apply] [--user <username>] [--limit <n>]\n"
        "  funes memory-migrate --undo [--user <username>]\n"
        "\n"
        "Queues the stored `User said … I replied …` memories, and the\n"
        "consolidated ones merged from them, for fact extraction\n"
        "(docs/memory-plan.md, phase 6). The running server's\n"
        "background pass does the work, one model call per exchange, when the\n"
        "machine is quiet; each log is archived (kept, out of recall) once its\n"
        "facts are stored. Reports without queueing unless --apply. --undo\n"
        "brings every archived log back; the extracted facts stay.\n"
        "\n"
        "  funes script-run <script> --user <username> [--param=value ...]\n"
        "\n"
        "Runs a library script the way run_script would for that account:\n"
        "in its workspace, with its manifest's checks and the same sandbox\n"
        "(src/core/sandbox.h). For checking a manifest's `access:` before an\n"
        "agent finds out it is wrong. The script really runs — side effects\n"
        "included.\n";
}

int cmd_cron_cleanup(const std::string& db_path, const std::vector<std::string>& args) {
    MemoryStore::CronCleanupOptions opt;   // dry_run defaults to true
    std::string username;

    for (size_t i = 0; i < args.size(); ++i) {
        // --dry-run is the default, but accepted so that typing it (the
        // cautious reflex) is not an "unknown option" error.
        if (args[i] == "--dry-run")      { opt.dry_run = true;  continue; }
        if (args[i] == "--apply")        { opt.dry_run = false; continue; }
        if (args[i] == "--drop-sessions"){ opt.drop_sessions = true; continue; }
        if (args[i] == "--user") {
            if (i + 1 >= args.size()) { std::cerr << "Missing value for --user\n"; return 2; }
            username = args[++i];
            continue;
        }
        std::cerr << "Unknown option: " << args[i] << "\n";
        usage();
        return 2;
    }

    if (!username.empty()) {
        UserStore users(db_path);
        auto user = users.find_by_username(username);
        if (!user) {
            std::cerr << "No such user: " << username << "\n";
            return 1;
        }
        opt.user_id = user->id;
    }

    // No embedding client: this only deletes, and forget() drops a vector by
    // memory id without needing to compute one. Handing it an embedder would
    // mean a cleanup that fails when the model host is down.
    MemoryStore memory(db_path, nullptr);
    const auto report = memory.cleanup_cron_history(opt);

    const char* verb = opt.dry_run ? "Would remove" : "Removed";
    std::cout << verb << ":\n"
              << "  " << report.memories << " auto-memor"
              << (report.memories == 1 ? "y" : "ies")
              << " written by a scheduled run\n";
    if (opt.drop_sessions)
        std::cout << "  " << report.sessions << " scheduled-run session(s)"
                  << " (" << report.turns << " turns)\n";
    else
        std::cout << "  (keeping " << report.sessions << " scheduled-run session(s), "
                  << report.turns << " turns — pass --drop-sessions to delete them)\n";

    if (opt.dry_run)
        std::cout << "\nDry run — nothing was deleted. Re-run with --apply.\n";
    return 0;
}

// The schedule is parsed here, not stored raw: a typo must be refused before it
// reaches a table the runner trusts, and next_run_at has to come from the same
// next_run_after the runner itself uses.
int cmd_cron_schedule(const std::string& db_path, const std::vector<std::string>& args) {
    if (args.size() != 2) { usage(); return 2; }
    int64_t id = 0;
    try { id = std::stoll(args[0]); } catch (const std::exception&) {
        std::cerr << "Not a job id: " << args[0] << "\n";
        return 2;
    }
    const std::string& schedule = args[1];
    cron::CronExpr expr;
    try { expr = cron::parse_cron_expr(schedule); } catch (const std::exception& e) {
        std::cerr << "Bad schedule: " << e.what() << "\n";
        return 2;
    }

    MemoryStore memory(db_path, nullptr);
    auto job = memory.get_cron_job(-1, id);
    if (!job) { std::cerr << "No such job: " << id << "\n"; return 1; }

    const time_t next = cron::next_run_after(expr, std::time(nullptr));
    if (!memory.reschedule_cron_job(-1, id, schedule, next)) {
        std::cerr << "No such job: " << id << "\n";
        return 1;
    }
    char when[32];
    std::strftime(when, sizeof when, "%a %F %R", std::localtime(&next));
    std::cout << "#" << id << " " << job->name << ": \"" << job->schedule
              << "\" -> \"" << schedule << "\", next run " << when << "\n";
    return 0;
}

int cmd_memory_migrate(const std::string& db_path, const std::vector<std::string>& args) {
    bool apply = false, undo = false;
    std::string username;
    size_t limit = 100000;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--dry-run") { apply = false; continue; }
        if (args[i] == "--apply")   { apply = true;  continue; }
        if (args[i] == "--undo")    { undo = true;   continue; }
        if (args[i] == "--user" || args[i] == "--limit") {
            if (i + 1 >= args.size()) { std::cerr << "Missing value for " << args[i] << "\n"; return 2; }
            const std::string v = args[++i];
            if (args[i - 1] == "--user") { username = v; continue; }
            try { limit = static_cast<size_t>(std::stoull(v)); } catch (const std::exception&) {
                std::cerr << "Not a number: " << v << "\n";
                return 2;
            }
            continue;
        }
        std::cerr << "Unknown option: " << args[i] << "\n";
        usage();
        return 2;
    }

    int64_t user_id = -1;
    if (!username.empty()) {
        UserStore users(db_path);
        auto user = users.find_by_username(username);
        if (!user) { std::cerr << "No such user: " << username << "\n"; return 1; }
        user_id = user->id;
    }

    // No embedder and no model: this only writes queue rows (or clears the
    // archive flag). The server's extraction thread does the model work.
    MemoryStore memory(db_path, nullptr);
    if (undo) {
        std::cout << "Restored " << memory.unarchive_migrated(user_id)
                  << " archived exchange log(s) to recall.\n";
        return 0;
    }
    const auto r = memory.enqueue_auto_migration(user_id, limit, !apply);
    std::cout << (apply ? "Queued " : "Would queue ") << r.candidates
              << " stored memories for fact extraction (" << r.notes
              << " of them merged notes)";
    if (r.unparsed) std::cout << " (" << r.unparsed << " auto memories not in that shape, left alone)";
    std::cout << ".\n";
    if (!apply)
        std::cout << "\nDry run — nothing was queued. Re-run with --apply.\n";
    else
        std::cout << "Already queued: " << memory.count_pending_exchanges()
                  << " pending. The server works through them in the background.\n";
    return 0;
}

int cmd_script_run(const std::string& db_path, const std::string& scripts_dir,
                   const std::string& workspace_dir, const std::vector<std::string>& args) {
    if (args.empty() || args[0].rfind("--", 0) == 0) { usage(); return 2; }
    const std::string name = args[0];
    std::string username;
    std::vector<std::string> params;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--user" && i + 1 < args.size()) { username = args[++i]; continue; }
        params.push_back(args[i]);
    }
    if (username.empty()) { std::cerr << "script-run needs --user <username>\n"; return 2; }
    UserStore users(db_path);
    const auto user = users.find_by_username(username);
    if (!user) { std::cerr << "No such user: " << username << "\n"; return 1; }

    ScriptSpec spec;
    const std::string err = load_script(scripts_dir, name, spec);
    if (!err.empty()) { std::cerr << err << "\n"; return 1; }

    // `--k=v` / `--flag`, typed by the manifest, so the arguments reach
    // build_argv as the model's JSON would.
    nlohmann::json arguments = nlohmann::json::object();
    for (const auto& p : params) {
        if (p.rfind("--", 0) != 0) { std::cerr << "Expected --param=value, got " << p << "\n"; return 2; }
        const size_t eq = p.find('=');
        const std::string key = p.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
        const std::string val = eq == std::string::npos ? "true" : p.substr(eq + 1);
        std::string type = "string";
        for (const auto& sp : spec.params) if (sp.name == key) type = sp.type;
        if (type == "boolean")     arguments[key] = (val == "true" || val == "1");
        else if (type == "number") arguments[key] = std::stod(val);
        else                       arguments[key] = val;
    }

    if (sandbox::enabled()) proc::set_trampoline("/proc/self/exe");
    ToolRegistry reg;
    register_script_tools(reg, workspace_dir, scripts_dir);
    ToolContext ctx{"cli", "script-run", "", "", user->id};
    ctx.scripts = {name};
    const ToolResult r = reg.call("run_script", {{"name", name}, {"arguments", arguments}}, ctx);
    std::cout << r.text << "\n";
    std::cerr << "[script-run] " << name << " as " << username << ": "
              << (r.error ? "error" : "ok") << ", sandbox "
              << (sandbox::enabled() ? "Landlock ABI " + std::to_string(sandbox::abi()) : std::string("off"))
              << "\n";
    return r.error ? 1 : 0;
}

} // namespace

bool is_maint_cli_command(int argc, char** argv) {
    if (argc < 2 || argv[1] == nullptr) return false;
    for (const char* c : COMMANDS)
        if (std::strcmp(argv[1], c) == 0) return true;
    return false;
}

int run_maint_cli(int argc, char** argv, const std::string& db_path,
                  const std::string& scripts_dir, const std::string& workspace_dir) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    try {
        if (cmd == "cron-cleanup")  return cmd_cron_cleanup(db_path, args);
        if (cmd == "cron-schedule") return cmd_cron_schedule(db_path, args);
        if (cmd == "memory-migrate") return cmd_memory_migrate(db_path, args);
        if (cmd == "script-run") return cmd_script_run(db_path, scripts_dir, workspace_dir, args);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    usage();
    return 2;
}

} // namespace funes
