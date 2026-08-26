// =============================================================================
// src/server/user_cli.cpp — admin account management (implementation)
// =============================================================================

#include "user_cli.h"
#include "../core/users.h"
#include <cstring>
#include <iostream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace funes {
namespace {

constexpr size_t MIN_PASSWORD_LENGTH = 8;

// Read a line with terminal echo off, restoring the previous terminal state
// even if the read fails. Falls back to a plain read when stdin is not a
// terminal (a pipe, e.g. in a provisioning script) — there is nothing to
// disable in that case, and refusing would make the command unscriptable.
std::string read_password(const std::string& prompt) {
    std::cout << prompt << std::flush;

    termios old{};
    const bool is_tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (is_tty) {
        termios noecho = old;
        noecho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
    }

    std::string line;
    std::getline(std::cin, line);

    if (is_tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        std::cout << "\n";  // the newline the user's Enter didn't echo
    }
    return line;
}

// Prompt twice and require a match, the way passwd(1) does — a typo in a
// password that is never echoed is otherwise only discovered at the next login.
std::string prompt_new_password() {
    std::string first = read_password("New password: ");
    if (first.size() < MIN_PASSWORD_LENGTH) {
        std::cerr << "Password must be at least " << MIN_PASSWORD_LENGTH
                  << " characters.\n";
        return "";
    }
    std::string again = read_password("Retype password: ");
    if (first != again) {
        std::cerr << "Passwords do not match.\n";
        return "";
    }
    return first;
}

void usage() {
    std::cerr <<
        "Usage:\n"
        "  funes useradd <username> [--admin] [--name \"Display Name\"]\n"
        "  funes userdel <username>\n"
        "  funes userlist\n"
        "  funes passwd  <username>\n"
        "  funes jid-map <chat_jid> <username>\n"
        "  funes jid-unmap <chat_jid>\n";
}

int cmd_useradd(UserStore& users, const std::vector<std::string>& args) {
    if (args.empty()) { usage(); return 2; }
    const std::string username = args[0];

    std::string role = UserStore::ROLE_MEMBER;
    std::string display = username;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--admin") role = UserStore::ROLE_ADMIN;
        else if (args[i] == "--name" && i + 1 < args.size()) display = args[++i];
        else { std::cerr << "Unknown option: " << args[i] << "\n"; usage(); return 2; }
    }

    if (users.find_by_username(username)) {
        std::cerr << "User '" << username << "' already exists.\n";
        return 1;
    }

    const std::string password = prompt_new_password();
    if (password.empty()) return 1;

    int64_t id = users.create_user(username, password, display, role);
    if (id == 0) {
        // The duplicate check above is a nicety for the error message; this is
        // the one that actually holds, since another process could have
        // created the same name in between.
        std::cerr << "Could not create user '" << username << "'.\n";
        return 1;
    }
    std::cout << "Created " << role << " '" << username << "' (id " << id << ").\n";
    return 0;
}

int cmd_userdel(UserStore& users, const std::vector<std::string>& args) {
    if (args.empty()) { usage(); return 2; }
    auto user = users.find_by_username(args[0]);
    if (!user) {
        std::cerr << "No such user: " << args[0] << "\n";
        return 1;
    }
    // Refuse to remove the last admin: an install with no admin cannot create
    // one back through any interface, since bootstrap only opens when there
    // are no users at all.
    if (user->is_admin()) {
        int admins = 0;
        for (const auto& u : users.list_users()) if (u.is_admin()) ++admins;
        if (admins <= 1) {
            std::cerr << "Refusing to delete the only admin account — "
                         "create another admin first.\n";
            return 1;
        }
    }
    if (!users.delete_user(user->id)) {
        std::cerr << "Could not delete user '" << args[0] << "'.\n";
        return 1;
    }
    std::cout << "Deleted '" << args[0] << "' (and its sessions and jid mappings).\n";
    return 0;
}

int cmd_userlist(UserStore& users) {
    auto all = users.list_users();
    if (all.empty()) {
        std::cout << "No users yet. Create the first admin with: funes useradd <name> --admin\n";
        return 0;
    }
    std::cout << "ID  ROLE    USERNAME             DISPLAY NAME\n";
    for (const auto& u : all) {
        std::cout << u.id << "   "
                  << (u.is_admin() ? "admin " : "member") << "  "
                  << u.username;
        for (size_t i = u.username.size(); i < 21; ++i) std::cout << ' ';
        std::cout << u.display_name << "\n";
    }
    return 0;
}

int cmd_passwd(UserStore& users, const std::vector<std::string>& args) {
    if (args.empty()) { usage(); return 2; }
    auto user = users.find_by_username(args[0]);
    if (!user) {
        std::cerr << "No such user: " << args[0] << "\n";
        return 1;
    }
    const std::string password = prompt_new_password();
    if (password.empty()) return 1;
    if (!users.set_password(user->id, password)) {
        std::cerr << "Could not change the password.\n";
        return 1;
    }
    // Existing sessions deliberately survive a password change here: this
    // command is the admin resetting a forgotten password, not a response to
    // a stolen cookie. Revoking sessions on password change is worth adding
    // once users can change their own.
    std::cout << "Password updated for '" << args[0] << "'.\n";
    return 0;
}

int cmd_jid_map(UserStore& users, const std::vector<std::string>& args) {
    if (args.size() < 2) { usage(); return 2; }
    const std::string& jid = args[0];
    auto user = users.find_by_username(args[1]);
    if (!user) {
        std::cerr << "No such user: " << args[1] << "\n";
        return 1;
    }
    if (!users.map_jid(jid, user->id)) {
        std::cerr << "Could not map '" << jid << "'.\n";
        return 1;
    }
    std::cout << "WhatsApp " << jid << " now acts as '" << args[1] << "'.\n";
    std::cout << "Note: this is identity, not permission — the sender must also be on "
                 "WHATSAPP_WHITELIST for the autoresponder to reply.\n";
    return 0;
}

int cmd_jid_unmap(UserStore& users, const std::vector<std::string>& args) {
    if (args.empty()) { usage(); return 2; }
    if (!users.unmap_jid(args[0])) {
        std::cerr << "No mapping for '" << args[0] << "'.\n";
        return 1;
    }
    std::cout << "Removed the mapping for " << args[0] << ".\n";
    return 0;
}

const char* const COMMANDS[] = {
    "useradd", "userdel", "userlist", "passwd", "jid-map", "jid-unmap"
};

} // namespace

bool is_user_cli_command(int argc, char** argv) {
    if (argc < 2 || argv[1] == nullptr) return false;
    for (const char* c : COMMANDS)
        if (std::strcmp(argv[1], c) == 0) return true;
    return false;
}

int run_user_cli(int argc, char** argv, const std::string& db_path) {
    if (argc < 2) { usage(); return 2; }
    const std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    try {
        UserStore users(db_path);
        if (cmd == "useradd")   return cmd_useradd(users, args);
        if (cmd == "userdel")   return cmd_userdel(users, args);
        if (cmd == "userlist")  return cmd_userlist(users);
        if (cmd == "passwd")    return cmd_passwd(users, args);
        if (cmd == "jid-map")   return cmd_jid_map(users, args);
        if (cmd == "jid-unmap") return cmd_jid_unmap(users, args);
    } catch (const std::exception& e) {
        std::cerr << "funes " << cmd << ": " << e.what() << "\n";
        return 1;
    }
    usage();
    return 2;
}

} // namespace funes
