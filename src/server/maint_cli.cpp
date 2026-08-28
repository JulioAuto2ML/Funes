// =============================================================================
// src/server/maint_cli.cpp — one-off database maintenance subcommands
// =============================================================================

#include "maint_cli.h"
#include "../core/memory.h"
#include "../core/users.h"
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace funes {
namespace {

const char* const COMMANDS[] = {"cron-cleanup"};

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
        "                   of what a run older than the last one did.\n";
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
        if (cmd == "cron-cleanup") return cmd_cron_cleanup(db_path, args);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    usage();
    return 2;
}

} // namespace funes
