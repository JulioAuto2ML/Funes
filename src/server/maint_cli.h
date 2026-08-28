// =============================================================================
// src/server/maint_cli.h — one-off database maintenance from the command line
// =============================================================================
//
// Separate from user_cli.h because these are not account management: they
// operate on stored conversations and memories, they are destructive, and
// they are run once after an upgrade rather than as part of running the box.
//
//   funes cron-cleanup [--apply] [--drop-sessions] [--user <username>]
//
// Removes the auto-memories a cron firing used to write back when it persisted
// like a conversation — the ones phrased `User said: "<the job's task>" —
// I replied: "..."`, which then got recalled into real conversations as things
// the person had said. New runs no longer write them (src/core/cron_runner.cpp
// uses Persist::TurnsOnly), so this is only for a database that predates that
// change.
//
// The per-run transcripts are reported but kept unless --drop-sessions: they
// are already hidden from the conversation list, and they are the only record
// of what a run older than the last one actually did.
//
// It goes through MemoryStore::cleanup_cron_history rather than SQL on
// purpose: a raw DELETE from `memories` leaves the vec0 index holding a vector
// whose row is gone, and nothing ever notices.
//
// Reports without deleting unless --apply. This is not reversible and there is
// no downgrade, so read the numbers first.
// =============================================================================

#pragma once
#include <string>

namespace funes {

// True if argv[1] names one of the subcommands above. Same shape as
// is_user_cli_command, so main() can try each in turn.
bool is_maint_cli_command(int argc, char** argv);

// Runs the subcommand. Returns the process exit code (0 on success).
// `db_path` is the already-resolved memory database.
int run_maint_cli(int argc, char** argv, const std::string& db_path);

} // namespace funes
