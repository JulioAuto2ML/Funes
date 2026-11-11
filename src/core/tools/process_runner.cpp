// =============================================================================
// src/core/tools/process_runner.cpp — fork/exec with timeout + output capture
// =============================================================================

#include "../funes_config.h"
#include "process_runner.h"
#include "../cgroup.h"
#include "../sandbox.h"
#include "../text_utils.h"
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace funes::proc {

namespace {

// Where `funes _exec` lives; empty = no trampoline, so no cgroup either.
std::string g_trampoline;

UsageObserver g_usage_observer;

} // namespace

void set_usage_observer(UsageObserver observer) { g_usage_observer = std::move(observer); }

void notify_usage(const std::vector<std::string>& argv, const funes::cgroup::Usage& usage) {
    if (g_usage_observer) g_usage_observer(argv, usage);
}

std::optional<funes::cgroup::Usage> Wrapped::finish() {
    std::optional<funes::cgroup::Usage> usage;
    if (scope) {
        usage = scope->finish();
        scope.reset();
    }
    if (!tmpdir.empty()) {
        std::error_code ec;
        fs::remove_all(tmpdir, ec);
        tmpdir.clear();
    }
    return usage;
}

// With cgroups on (cgroup.h) the child is wrapped: it execs the trampoline,
// which joins the child's own cgroup, confines itself with `policy`
// (sandbox.h) and then execs argv. A temp directory of the call's own goes
// with the sandbox: it does not grant /tmp, which every account's children
// would otherwise share.
std::unique_ptr<Wrapped> wrap(const std::vector<std::string>& argv,
                              const std::optional<funes::cgroup::Limits>& limits,
                              const std::optional<funes::sandbox::Policy>& policy) {
    auto w = std::make_unique<Wrapped>();
    if (!g_trampoline.empty())
        w->scope = funes::cgroup::Scope::create(argv.empty() ? "exec" : argv[0],
                                                limits ? *limits : funes::cgroup::default_limits());

    std::vector<std::string> confine;
    if (!g_trampoline.empty() && policy && funes::sandbox::enabled()) {
        funes::sandbox::Policy p = *policy;
        std::string tmpl = (fs::temp_directory_path() / "funes-exec-XXXXXX").string();
        if (mkdtemp(tmpl.data())) {
            w->tmpdir = tmpl;
            p.write.push_back(w->tmpdir);
        }
        confine = funes::sandbox::trampoline_args(p);
    }

    if (w->scope || !confine.empty()) {
        w->argv = {g_trampoline, "_exec"};
        if (w->scope) w->argv.push_back("--cgroup=" + w->scope->path());
        w->argv.insert(w->argv.end(), confine.begin(), confine.end());
        w->argv.push_back("--");
    }
    w->argv.insert(w->argv.end(), argv.begin(), argv.end());
    return w;
}

namespace {

// Runs argv (argv[0] resolved against PATH) with `env_strings` as its whole
// environment. Captures stdout and stderr — merged into one stream, or apart
// when `split` is set — enforces the timeout, and cleans up the truncation
// boundary.
//
// The child is wrapped by wrap() above. Everything — the trampoline's argv,
// the envp — is built here, before the fork, so the fork child still does
// nothing but dup2, chdir and exec, the only things safe in the child of a
// multi-threaded process.
Result run_forked(const std::vector<std::string>& argv, std::vector<std::string> env_strings,
                  const fs::path& cwd, int timeout_seconds, size_t max_output_bytes,
                  bool split, const std::optional<funes::cgroup::Limits>& limits,
                  const std::optional<funes::sandbox::Policy>& policy) {
    Result result;

    const std::unique_ptr<Wrapped> w = wrap(argv, limits, policy);
    if (!w->tmpdir.empty()) {
        env_strings.erase(std::remove_if(env_strings.begin(), env_strings.end(),
            [](const std::string& e) { return e.rfind("TMPDIR=", 0) == 0; }),
            env_strings.end());
        env_strings.push_back("TMPDIR=" + w->tmpdir);
    }
    const std::vector<std::string>& run = w->argv;
    funes::cgroup::Scope* const scope = w->scope.get();
    std::vector<char*> c_argv;
    c_argv.reserve(run.size() + 1);
    for (const auto& a : run) c_argv.push_back(const_cast<char*>(a.c_str()));
    c_argv.push_back(nullptr);
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& e : env_strings) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

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
        // execvp resolves argv[0] against the parent's PATH (the one it was
        // started with), then the child runs with the filtered block.
        environ = envp.data();
        execvp(c_argv[0], c_argv.data());
        _exit(127);
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
            // The cgroup holds everything the child started, including what
            // left the process group; the group kill covers no-cgroup runs
            // and the instant before the trampoline has joined.
            if (scope) scope->kill();
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
    result.usage = w->finish();
    if (result.usage) notify_usage(argv, *result.usage);

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
    if (const char* more = funes::config_value("FUNES_CHILD_ENV")) {
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
    // A name the operator listed in FUNES_CHILD_ENV whose value lives in
    // funes.local or a credential: since 7.8 that is config, not environment
    // (funes_config.h), so the loop above cannot see it.
    for (const auto& n : extra_names) {
        if (std::getenv(n.c_str()) || overridden(n)) continue;
        if (const char* v = funes::config_value(n)) out.push_back(n + "=" + v);
    }
    for (const auto& kv : extra_env) out.push_back(kv.first + "=" + kv.second);
    return out;
}

void set_trampoline(const std::string& exe) { g_trampoline = exe; }

int trampoline_main(int argc, char** argv) {
    // funes _exec [--cgroup=DIR] [--sandbox --ro=P --rx=P --rw=P --connect=N]
    //             -- PROGRAM ARGS...
    std::string cgroup;
    bool sandboxed = false;
    funes::sandbox::Policy policy;
    int i = 2;
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") { ++i; break; }
        if (a.rfind("--cgroup=", 0) == 0) cgroup = a.substr(9);
        else if (a == "--sandbox")           sandboxed = true;
        else if (a.rfind("--ro=", 0) == 0)  policy.read.push_back(a.substr(5));
        else if (a.rfind("--rx=", 0) == 0)  policy.exec.push_back(a.substr(5));
        else if (a.rfind("--rw=", 0) == 0)  policy.write.push_back(a.substr(5));
        else if (a.rfind("--connect=", 0) == 0) policy.connect.push_back(std::atoi(a.c_str() + 10));
        else { std::fprintf(stderr, "funes _exec: unknown option %s\n", a.c_str()); return 126; }
    }
    if (i >= argc) { std::fprintf(stderr, "funes _exec: no program\n"); return 126; }
    if (!cgroup.empty()) {
        // Joining fails closed: the parent made this cgroup to hold limits,
        // and a child running outside it would run without them.
        const std::string procs = cgroup + "/cgroup.procs";
        FILE* f = std::fopen(procs.c_str(), "we");
        const bool ok = f && std::fputs("0", f) >= 0;
        if (!f || std::fclose(f) != 0 || !ok) {
            std::fprintf(stderr, "funes _exec: cannot join %s: %s\n", cgroup.c_str(),
                         std::strerror(errno));
            return 126;
        }
    }
    // After the cgroup is joined (that write is outside any sandbox), before
    // exec. Fails closed for the same reason as the join.
    if (sandboxed) {
        const std::string err = funes::sandbox::apply(policy);
        if (!err.empty()) {
            std::fprintf(stderr, "funes _exec: sandbox: %s\n", err.c_str());
            return 126;
        }
    }
    execvp(argv[i], argv + i);
    std::fprintf(stderr, "funes _exec: %s: %s\n", argv[i], std::strerror(errno));
    return 127;
}

Result run_argv(const std::vector<std::string>& argv, const fs::path& cwd,
                int timeout_seconds, size_t max_output_bytes,
                const std::vector<std::pair<std::string, std::string>>& extra_env,
                bool separate_stderr, const std::optional<funes::cgroup::Limits>& limits,
                const std::optional<funes::sandbox::Policy>& policy) {
    if (argv.empty()) return {-1, "run_argv: empty argv", {}, false, {}};
    return run_forked(argv, child_environment(extra_env), cwd, timeout_seconds,
                      max_output_bytes, separate_stderr, limits, policy);
}

Result run_shell_command(const std::string& command, const fs::path& cwd,
                         int timeout_seconds, size_t max_output_bytes,
                         const std::optional<funes::cgroup::Limits>& limits,
                         const std::optional<funes::sandbox::Policy>& policy) {
    // Same filtered block as run_argv. A shell is unconfined by nature, but
    // "unconfined" is about what it can reach on the machine — not a reason
    // to also hand it the server's own service token, which `env` would then
    // print straight into the model's context.
    return run_forked({"/bin/sh", "-c", command}, child_environment(), cwd, timeout_seconds,
                      max_output_bytes, false, limits, policy);
}

} // namespace funes::proc
