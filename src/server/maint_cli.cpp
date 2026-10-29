// =============================================================================
// src/server/maint_cli.cpp — database maintenance subcommands
// =============================================================================

#include "maint_cli.h"
#include "../core/cron_schedule.h"
#include "../core/memory.h"
#include "../core/users.h"
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace funes {
namespace {

const char* const COMMANDS[] = {"cron-cleanup", "cron-schedule"};

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
        "the runner reads the table on every tick.\n";
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

} // namespace

bool is_maint_cli_command(int argc, char** argv) {
    if (argc < 2 || argv[1] == nullptr) return false;
    for (const char* c : COMMANDS)
        if (std::strcmp(argv[1], c) == 0) return true;
    return false;
}

int run_maint_cli(int argc, char** argv, const std::string& db_path) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    try {
        if (cmd == "cron-cleanup")  return cmd_cron_cleanup(db_path, args);
        if (cmd == "cron-schedule") return cmd_cron_schedule(db_path, args);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    usage();
    return 2;
}

} // namespace funes
