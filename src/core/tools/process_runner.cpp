// =============================================================================
// src/core/tools/process_runner.cpp — fork/exec with timeout + output capture
// =============================================================================

#include "process_runner.h"
#include "../text_utils.h"
#include <algorithm>
#include <cstdlib>
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
// return on success; _exit on failure). Captures stdout and stderr — merged
// into one stream, or apart when `split` is set — enforces the timeout, and
// cleans up the truncation boundary.
Result run_forked(const std::function<void()>& exec_in_child, const fs::path& cwd,
                  int timeout_seconds, size_t max_output_bytes, bool split = false) {
    Result result;

    // outfd carries stdout; errfd carries stderr only when the caller asked
    // for them apart, and is otherwise the same pipe (one fd, two dup2s).
    int outfd[2];
    if (pipe(outfd) != 0) {
        result.output = "pipe() failed";
        return result;
    }
    int errfd[2] = {outfd[0], outfd[1]};
    if (split && pipe(errfd) != 0) {
        close(outfd[0]);
        close(outfd[1]);
        result.output = "pipe() failed";
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(outfd[0]);
        close(outfd[1]);
        if (split) { close(errfd[0]); close(errfd[1]); }
        result.output = "fork() failed";
        return result;
    }

    if (pid == 0) {
        // Child: own process group so a timeout can kill the whole tree,
        // stdout and stderr go to their pipe(s), cwd is set before exec.
        setpgid(0, 0);
        dup2(outfd[1], STDOUT_FILENO);
        dup2(errfd[1], STDERR_FILENO);
        close(outfd[0]);
        close(outfd[1]);
        if (split) { close(errfd[0]); close(errfd[1]); }
        if (::chdir(cwd.c_str()) != 0) _exit(126);
        exec_in_child();
        _exit(127);  // only reached if exec_in_child's exec call failed
    }

    // Parent.
    setpgid(pid, pid);  // avoid a race with the child's own setpgid call
    close(outfd[1]);
    if (split) close(errfd[1]);
    for (int fd : {outfd[0], split ? errfd[0] : -1}) {
        if (fd < 0) continue;
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    auto drain_one = [&](int fd, std::string& into) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (into.size() < max_output_bytes)
                into.append(buf, static_cast<size_t>(n));
        }
    };
    auto drain = [&] {
        drain_one(outfd[0], result.output);
        if (split) drain_one(errfd[0], result.stderr_output);
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

        struct pollfd pfds[2] = {{outfd[0], POLLIN, 0}, {errfd[0], POLLIN, 0}};
        const int wait_ms = static_cast<int>(std::min<std::chrono::milliseconds::rep>(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count(), 200));
        poll(pfds, split ? 2 : 1, std::max(wait_ms, 0));
    }

    drain();
    close(outfd[0]);
    if (split) close(errfd[0]);

    if (!result.timed_out && WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    // The byte cap above can land mid-character; trim back to a clean UTF-8
    // boundary before the caller ever sees this text.
    for (std::string* stream : {&result.output, &result.stderr_output}) {
        if (stream->size() < max_output_bytes) continue;
        funes::truncate_utf8_safe(*stream, max_output_bytes);
        *stream += "\n[output truncated at " + std::to_string(max_output_bytes / 1024) + " KB]";
    }
    return result;
}

} // namespace

std::vector<std::string> child_environment(
    const std::vector<std::pair<std::string, std::string>>& extra_env) {
    // Exact names a program needs to start and find its libraries, and the
    // prefixes for locale and desktop-session plumbing. Deliberately no
    // FUNES_* entry: the server's own configuration is not the child's
    // business, and its secrets least of all.
    static const char* const kExact[] = {
        "PATH", "HOME", "USER", "LOGNAME", "SHELL", "TERM", "TMPDIR", "TZ",
        "LANG", "LANGUAGE",
        "PYTHONPATH", "PYTHONUNBUFFERED", "PYTHONDONTWRITEBYTECODE", "VIRTUAL_ENV",
        "SSL_CERT_FILE", "SSL_CERT_DIR", "REQUESTS_CA_BUNDLE", "CURL_CA_BUNDLE",
        "http_proxy", "https_proxy", "no_proxy", "HTTP_PROXY", "HTTPS_PROXY", "NO_PROXY",
    };
    static const char* const kPrefix[] = {"LC_", "XDG_"};

    std::vector<std::string> extra_names;
    if (const char* more = std::getenv("FUNES_CHILD_ENV")) {
        std::string s(more);
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t end = s.find(',', pos);
            if (end == std::string::npos) end = s.size();
            std::string name = s.substr(pos, end - pos);
            name.erase(0, name.find_first_not_of(" \t"));
            if (!name.empty()) name.erase(name.find_last_not_of(" \t") + 1);
            if (!name.empty()) extra_names.push_back(name);
            pos = end + 1;
        }
    }

    auto passes = [&](const std::string& key) {
        for (const char* k : kExact)  if (key == k) return true;
        for (const char* p : kPrefix) if (key.rfind(p, 0) == 0) return true;
        for (const auto& n : extra_names) if (key == n) return true;
        return false;
    };
    auto overridden = [&](const std::string& key) {
        return std::any_of(extra_env.begin(), extra_env.end(),
                           [&key](const auto& kv) { return kv.first == key; });
    };

    std::vector<std::string> out;
    for (char** e = environ; e && *e; ++e) {
        const std::string entry(*e);
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = entry.substr(0, eq);
        if (passes(key) && !overridden(key)) out.push_back(entry);
    }
    for (const auto& kv : extra_env) out.push_back(kv.first + "=" + kv.second);
    return out;
}

namespace {
// The "KEY=value" strings must outlive the exec; the vector of pointers is
// what execve wants. Both live in the parent's frame for the whole call.
std::vector<char*> envp_of(std::vector<std::string>& env_strings) {
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);
    return envp;
}
} // namespace

Result run_argv(const std::vector<std::string>& argv, const fs::path& cwd,
                int timeout_seconds, size_t max_output_bytes,
                const std::vector<std::pair<std::string, std::string>>& extra_env,
                bool separate_stderr) {
    if (argv.empty()) return {-1, "run_argv: empty argv", {}, false};

    // Built here, in the parent: everything the child does after fork() is a
    // pointer assignment and an exec, which is the only thing guaranteed safe
    // in the child of a multi-threaded process (the server is one).
    std::vector<std::string> env_strings = child_environment(extra_env);
    std::vector<char*>       envp        = envp_of(env_strings);

    return run_forked([&argv, &envp] {
        std::vector<char*> c_argv;
        c_argv.reserve(argv.size() + 1);
        for (const auto& a : argv) c_argv.push_back(const_cast<char*>(a.c_str()));
        c_argv.push_back(nullptr);
        // execvp resolves argv[0] against the parent's PATH (the one it was
        // started with), then the child runs with the filtered block.
        environ = envp.data();
        execvp(c_argv[0], c_argv.data());
    }, cwd, timeout_seconds, max_output_bytes, separate_stderr);
}

Result run_shell_command(const std::string& command, const fs::path& cwd,
                         int timeout_seconds, size_t max_output_bytes) {
    // Same filtered block as run_argv. A shell is unconfined by nature, but
    // "unconfined" is about what it can reach on the machine — not a reason
    // to also hand it the server's own service token, which `env` would then
    // print straight into the model's context.
    std::vector<std::string> env_strings = child_environment();
    std::vector<char*>       envp        = envp_of(env_strings);
    char* const sh_argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
                             const_cast<char*>(command.c_str()), nullptr};
    return run_forked([&sh_argv, &envp] {
        execve("/bin/sh", sh_argv, envp.data());
    }, cwd, timeout_seconds, max_output_bytes);
}

} // namespace funes::proc
