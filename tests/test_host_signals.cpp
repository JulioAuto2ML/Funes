// =============================================================================
// tests/test_host_signals.cpp — sd_notify and the background-work gate
// =============================================================================
// The two halves of OS plan phase 0 that talk to the host rather than to a
// model: telling systemd we are ready and alive, and knowing when to stay out
// of the way. Both are tested against the real interfaces — a real datagram
// socket standing in for systemd, real files in PSI format.

#include "agent.h"
#include "busy_gate.h"
#include "httplib.h"
#include "memory.h"
#include "permissions.h"
#include "sd_notify.h"
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

// A bound datagram socket playing systemd's notify socket.
struct FakeSystemd {
    int fd = -1;
    std::string path;
    explicit FakeSystemd(bool abstract_ns) {
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        const std::string name = "funes-test-notify-" + std::to_string(getpid());
        socklen_t len;
        if (abstract_ns) {
            path = "@" + name;
            a.sun_path[0] = '\0';
            std::memcpy(a.sun_path + 1, name.data(), name.size());
            len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        } else {
            path = "/tmp/" + name + ".sock";
            unlink(path.c_str());
            std::memcpy(a.sun_path, path.data(), path.size());
            len = static_cast<socklen_t>(sizeof a);
        }
        bind(fd, reinterpret_cast<sockaddr*>(&a), len);
        timeval tv{0, 300 * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setenv("NOTIFY_SOCKET", path.c_str(), 1);
    }
    ~FakeSystemd() {
        close(fd);
        if (path[0] == '/') unlink(path.c_str());
        unsetenv("NOTIFY_SOCKET");
    }
    // Next message, or "" on timeout.
    std::string recv_one() {
        char buf[256];
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        return n > 0 ? std::string(buf, static_cast<size_t>(n)) : "";
    }
};

std::string write_psi(const std::string& name, double some_avg10) {
    const std::string p = "/tmp/funes-test-psi-" + std::to_string(getpid()) + "-" + name;
    std::ofstream(p) << "some avg10=" << some_avg10 << " avg60=0.00 avg300=0.00 total=1\n"
                     << "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n";
    return p;
}

} // namespace

int test_notify_reaches_path_and_abstract_sockets() {
    unsetenv("NOTIFY_SOCKET");
    CHECK(!funes::sd::notify("READY=1"));   // outside systemd: a no-op, not an error

    for (bool abstract_ns : {false, true}) {
        FakeSystemd sd(abstract_ns);
        CHECK(funes::sd::notify("READY=1"));
        CHECK(sd.recv_one() == "READY=1");
    }
    return 0;
}

int test_watchdog_interval_reads_systemds_variables() {
    using std::chrono::microseconds;
    unsetenv("WATCHDOG_USEC");
    unsetenv("WATCHDOG_PID");
    CHECK(funes::sd::watchdog_interval() == microseconds(0));

    setenv("WATCHDOG_USEC", "60000000", 1);   // WatchdogSec=60
    CHECK(funes::sd::watchdog_interval() == microseconds(30000000));

    setenv("WATCHDOG_PID", "1", 1);           // meant for another process
    CHECK(funes::sd::watchdog_interval() == microseconds(0));
    setenv("WATCHDOG_PID", std::to_string(getpid()).c_str(), 1);
    CHECK(funes::sd::watchdog_interval() == microseconds(30000000));

    unsetenv("WATCHDOG_USEC");
    unsetenv("WATCHDOG_PID");
    return 0;
}

// The point of the watchdog: a ping is a claim that the server answers, so a
// failing probe must withhold it.
int test_watchdog_pings_only_while_probe_answers() {
    FakeSystemd sd(false);
    std::atomic<bool> healthy{true};
    {
        funes::sd::Watchdog wd(std::chrono::milliseconds(50), [&] { return healthy.load(); });
        CHECK(sd.recv_one() == "WATCHDOG=1");
        healthy = false;
        // Drain anything sent before the flip, then expect silence.
        while (!sd.recv_one().empty()) {}
        CHECK(sd.recv_one().empty());
        healthy = true;
        CHECK(sd.recv_one() == "WATCHDOG=1");
    }
    // Destroyed: stops pinging.
    while (!sd.recv_one().empty()) {}
    CHECK(sd.recv_one().empty());
    return 0;
}

int test_psi_parsing() {
    using funes::load::psi_some_avg10;
    const std::string p = write_psi("parse", 12.5);
    CHECK(psi_some_avg10(p) == 12.5);
    CHECK(psi_some_avg10("/nonexistent/pressure") < 0);
    unlink(p.c_str());
    return 0;
}

int test_quiet_means_no_runs_and_low_pressure() {
    using namespace funes::load;
    QuietOptions opt;
    opt.cpu_path = write_psi("cpu", 1.0);
    opt.io_path  = write_psi("io", 1.0);
    opt.poll     = std::chrono::seconds(0);
    opt.max_wait = std::chrono::seconds(0);

    CHECK(runs_in_flight() == 0);
    CHECK(is_quiet(opt));
    {
        RunInFlight run;
        CHECK(runs_in_flight() == 1);
        CHECK(!is_quiet(opt));
        CHECK(!wait_until_quiet(opt));   // gives up at max_wait rather than hanging
    }
    CHECK(is_quiet(opt));

    write_psi("io", 55.0);               // someone else is hammering the disk
    CHECK(!is_quiet(opt));

    // A host without PSI is not "always busy".
    opt.io_path  = "/nonexistent/io";
    opt.cpu_path = "/nonexistent/cpu";
    CHECK(is_quiet(opt));

    unlink(("/tmp/funes-test-psi-" + std::to_string(getpid()) + "-cpu").c_str());
    unlink(("/tmp/funes-test-psi-" + std::to_string(getpid()) + "-io").c_str());
    return 0;
}

// Every agent run counts as in flight while it talks to the model, and stops
// counting when it ends — including by throwing, which is how a dead backend
// ends one. The mock backend answers 500 after noting what it saw.
int test_agent_run_counts_as_in_flight() {
    namespace fs = std::filesystem;
    std::atomic<int> seen{-1};
    httplib::Server llm;
    llm.Post(R"(/.*)", [&](const httplib::Request&, httplib::Response& res) {
        seen = funes::load::runs_in_flight();
        res.status = 500;
        res.set_content("{\"error\":\"mock\"}", "application/json");
    });
    const int port = llm.bind_to_any_port("127.0.0.1");
    std::thread th([&] { llm.listen_after_bind(); });
    for (int i = 0; i < 50 && !llm.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const fs::path db = fs::temp_directory_path() / "funes_test_host_signals.db";
    fs::remove(db);
    {
        MemoryStore memory(db.string(), nullptr);
        ToolRegistry tools;
        AgentDefaults defaults;
        defaults.llm_url = "http://127.0.0.1:" + std::to_string(port);
        AgentConfig cfg;
        cfg.name = "funes";
        cfg.system_prompt = "test";
        FunesAgent agent(cfg, tools, memory, defaults);
        try {
            agent.run("hello", "s1", 1, funes::Permissions::unrestricted(), nullptr, {},
                      Persist::None);
        } catch (const std::exception&) {}
    }
    llm.stop();
    th.join();
    fs::remove(db);

    CHECK(seen.load() == 1);
    CHECK(funes::load::runs_in_flight() == 0);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_notify_reaches_path_and_abstract_sockets();
    rc |= test_watchdog_interval_reads_systemds_variables();
    rc |= test_watchdog_pings_only_while_probe_answers();
    rc |= test_psi_parsing();
    rc |= test_quiet_means_no_runs_and_low_pressure();
    rc |= test_agent_run_counts_as_in_flight();
    if (rc == 0) std::cout << "test_host_signals: all tests passed\n";
    return rc;
}
