// =============================================================================
// src/core/tools/process_runner.cpp — fork/exec with timeout + output capture
// =============================================================================

#include "process_runner.h"
#include "../text_utils.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace funes::proc {

namespace {

// Runs whatever `exec_in_child` execs into (it must chdir + exec and never
// return on success; _exit on failure). Captures stdout+stderr, enforces
// the timeout, and cleans up the truncation boundary.
Result run_forked(const std::function<void()>& exec_in_child, const fs::path& cwd,
                  int timeout_seconds, size_t max_output_bytes) {
    Result result;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        result.output = "pipe() failed";
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        result.output = "fork() failed";
        return result;
    }

    if (pid == 0) {
        // Child: own process group so a timeout can kill the whole tree,
        // stdout+stderr both go to the pipe, cwd is set before exec.
        setpgid(0, 0);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        if (::chdir(cwd.c_str()) != 0) _exit(126);
        exec_in_child();
        _exit(127);  // only reached if exec_in_child's exec call failed
    }

    // Parent.
    setpgid(pid, pid);  // avoid a race with the child's own setpgid call
    close(pipefd[1]);
    const int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    auto drain = [&] {
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            if (result.output.size() < max_output_bytes)
                result.output.append(buf, static_cast<size_t>(n));
        }
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    int status = 0;
    while (true) {
        drain();

        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;

        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0)) {
            result.timed_out = true;
            kill(-pid, SIGKILL);  // negative pid → whole process group
            waitpid(pid, &status, 0);
            break;
        }

        struct pollfd pfd{pipefd[0], POLLIN, 0};
        const int wait_ms = static_cast<int>(std::min<std::chrono::milliseconds::rep>(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count(), 200));
        poll(&pfd, 1, std::max(wait_ms, 0));
    }

    drain();
    close(pipefd[0]);

    if (!result.timed_out && WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    if (result.output.size() >= max_output_bytes) {
        // The byte cap above can land mid-character; trim back to a clean
        // UTF-8 boundary before the caller ever sees this text.
        funes::truncate_utf8_safe(result.output, max_output_bytes);
        result.output += "\n[output truncated at " + std::to_string(max_output_bytes / 1024) + " KB]";
    }
    return result;
}

} // namespace

Result run_argv(const std::vector<std::string>& argv, const fs::path& cwd,
                int timeout_seconds, size_t max_output_bytes,
                const std::vector<std::pair<std::string, std::string>>& extra_env) {
    if (argv.empty()) return {-1, "run_argv: empty argv", false};

    // Built here, in the parent: everything the child does after fork() is a
    // pointer assignment and an exec, which is the only thing guaranteed safe
    // in the child of a multi-threaded process (the server is one).
    std::vector<std::string> env_strings;
    std::vector<char*>       envp;
    if (!extra_env.empty()) {
        for (char** e = environ; e && *e; ++e) {
            const std::string entry(*e);
            const std::string key = entry.substr(0, entry.find('='));
            const bool overridden = std::any_of(
                extra_env.begin(), extra_env.end(),
                [&key](const auto& kv) { return kv.first == key; });
            if (!overridden) env_strings.push_back(entry);
        }
        for (const auto& kv : extra_env)
            env_strings.push_back(kv.first + "=" + kv.second);
        envp.reserve(env_strings.size() + 1);
        for (auto& s : env_strings) envp.push_back(const_cast<char*>(s.c_str()));
        envp.push_back(nullptr);
    }

    return run_forked([&argv, &envp] {
        std::vector<char*> c_argv;
        c_argv.reserve(argv.size() + 1);
        for (const auto& a : argv) c_argv.push_back(const_cast<char*>(a.c_str()));
        c_argv.push_back(nullptr);
        if (!envp.empty()) environ = const_cast<char**>(envp.data());
        execvp(c_argv[0], c_argv.data());
    }, cwd, timeout_seconds, max_output_bytes);
}

Result run_shell_command(const std::string& command, const fs::path& cwd,
                         int timeout_seconds, size_t max_output_bytes) {
    return run_forked([&command] {
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
    }, cwd, timeout_seconds, max_output_bytes);
}

} // namespace funes::proc
