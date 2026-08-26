// =============================================================================
// src/server/user_cli.h — admin account management from the command line
// =============================================================================
//
// User management is CLI-only by design (docs/dev-plan-users-permissions.md):
// this is a household appliance, not a SaaS, so there is no self-registration
// and no user-CRUD API. The admin creates accounts here and hands out
// credentials. That keeps the authenticated HTTP surface to one endpoint.
//
//   funes useradd <username> [--admin] [--name "Display Name"]
//   funes userdel <username>
//   funes userlist
//   funes passwd  <username>
//   funes jid-map <chat_jid> <username>     — let a WhatsApp number act as a user
//   funes jid-unmap <chat_jid>
//
// Passwords are always prompted for, never taken as an argument: an argv
// password lands in shell history and is visible in `ps` to every user on the
// box for as long as the command runs.
// =============================================================================

#pragma once
#include <string>

namespace funes {

// True if argv[1] names one of the subcommands above. Lets main() decide
// whether to run a command or start the server, without duplicating the list.
bool is_user_cli_command(int argc, char** argv);

// Runs the subcommand. Returns the process exit code (0 on success).
// `db_path` is the already-resolved memory database.
int run_user_cli(int argc, char** argv, const std::string& db_path);

} // namespace funes
