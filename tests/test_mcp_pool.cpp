// tests/test_mcp_pool.cpp — MCP stdio servers kept alive between runs (mcp_pool.h)
//
// A real stdio server (a few lines of Python) behind a real pool: a lease
// hands back the same process next time, two at once get two, a dead or
// failed server is never handed out again, idle ones are stopped, and a
// changed configuration gets a new process. Plus the two vendored-client
// patches the pool leans on: a server that exits is not "running", and a call
// waiting on it fails at once rather than after the 60-second timeout.

#include "agent_config.h"
#include "mcp_pool.h"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

const fs::path kBase = fs::temp_directory_path() / ("funes_test_mcp_pool_" + std::to_string(getpid()));

const char* kServer = R"py(import json, os, sys, time
for line in sys.stdin:
    m = json.loads(line)
    if "id" not in m: continue
    r = {}
    if m["method"] == "initialize":
        r = {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
             "serverInfo": {"name": "pool", "version": "1"}}
    elif m["method"] == "tools/list":
        r = {"tools": [{"name": n, "description": n, "inputSchema": {"type": "object"}}
                       for n in ("pid", "die", "env")]}
    elif m["method"] == "tools/call":
        name = m["params"]["name"]
        if name == "die":
            os._exit(0)          # no reply: the caller is left waiting
        text = str(os.getpid()) if name == "pid" else os.environ.get("POOL_TAG", "")
        r = {"content": [{"type": "text", "text": text}]}
    sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": m["id"], "result": r}) + "\n")
    sys.stdout.flush()
)py";

McpServerConfig server(const std::string& tag = "a") {
    McpServerConfig srv;
    srv.name = "pool";
    srv.command = "python3 " + (kBase / "server.py").string();
    srv.env = {{"POOL_TAG", tag}};
    return srv;
}

funes::McpLease lease(funes::McpPool& pool, const McpServerConfig& srv = server()) {
    std::string error;
    auto l = pool.acquire(srv, "test", "test", error);
    if (!l) std::cerr << "acquire failed: " << error << "\n";
    return l;
}

std::string call(const funes::McpLease& l, const std::string& tool) {
    return l.client()->call_tool(tool)["content"][0]["text"].get<std::string>();
}

bool alive(pid_t pid) { return ::kill(pid, 0) == 0; }

int test_a_returned_server_is_reused() {
    funes::McpPool pool(true, 600, 2);
    std::string first;
    {
        auto l = lease(pool);
        CHECK(l);
        CHECK(!l.reused());
        CHECK(l.tools().size() == 3);
        first = call(l, "pid");
    }
    CHECK(pool.stats().idle == 1);
    auto l = lease(pool);
    CHECK(l.reused());
    CHECK(l.tools().size() == 3);         // the tool list travels with it
    CHECK(call(l, "pid") == first);
    CHECK(pool.stats().started == 1);
    CHECK(pool.stats().reused == 1);
    std::cout << "  ok: a returned server is handed to the next run, tools and all\n";
    return 0;
}

int test_concurrent_leases_get_their_own_server() {
    funes::McpPool pool(true, 600, 1);
    {
        auto a = lease(pool);
        auto b = lease(pool);
        CHECK(a && b);
        CHECK(call(a, "pid") != call(b, "pid"));   // never one stdin for two runs
    }
    // Only idle_max of them stay.
    CHECK(pool.stats().idle == 1);
    CHECK(pool.stats().started == 2);
    std::cout << "  ok: two runs at once get two servers; the spare is stopped\n";
    return 0;
}

int test_a_dead_server_is_not_handed_out() {
    funes::McpPool pool(true, 600, 2);
    std::string before;
    {
        auto l = lease(pool);
        before = call(l, "pid");
        // The vendored client used to wait out its 60-second timeout here and
        // then report the server as still running.
        const auto t0 = std::chrono::steady_clock::now();
        bool threw = false;
        try { call(l, "die"); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
        for (int i = 0; i < 100 && l.client()->is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!l.client()->is_running());
    }
    CHECK(pool.stats().idle == 0);        // not returned
    auto l = lease(pool);
    CHECK(!l.reused());
    CHECK(call(l, "pid") != before);
    std::cout << "  ok: a server that died mid-call fails the call at once and is not reused\n";
    return 0;
}

int test_a_server_killed_while_idle_is_replaced() {
    funes::McpPool pool(true, 600, 2);
    pid_t pid;
    {
        auto l = lease(pool);
        pid = std::stoi(call(l, "pid"));
    }
    ::kill(pid, SIGKILL);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto l = lease(pool);
    CHECK(l);
    CHECK(!l.reused());
    CHECK(std::stoi(call(l, "pid")) != pid);
    std::cout << "  ok: a server that died while idle is replaced, not handed out\n";
    return 0;
}

int test_a_discarded_lease_is_stopped() {
    funes::McpPool pool(true, 600, 2);
    pid_t pid;
    {
        auto l = lease(pool);
        pid = std::stoi(call(l, "pid"));
        l.discard();                      // what dispatch_tool does when a call throws
    }
    CHECK(pool.stats().idle == 0);
    CHECK(!alive(pid));
    std::cout << "  ok: a lease whose call failed is stopped, not returned\n";
    return 0;
}

int test_idle_servers_are_reaped() {
    funes::McpPool pool(true, 0, 2);
    pid_t pid;
    {
        auto l = lease(pool);
        pid = std::stoi(call(l, "pid"));
    }
    CHECK(pool.stats().idle == 1);
    pool.reap();
    CHECK(pool.stats().idle == 0);
    CHECK(!alive(pid));
    std::cout << "  ok: a server idle past the limit is stopped\n";
    return 0;
}

int test_configuration_is_part_of_the_key() {
    funes::McpPool pool(true, 600, 2);
    { auto l = lease(pool, server("a")); CHECK(call(l, "env") == "a"); }
    auto l = lease(pool, server("b"));
    CHECK(!l.reused());
    CHECK(call(l, "env") == "b");
    McpServerConfig wider = server("a");
    wider.access.connect = {443};
    CHECK(funes::McpPool::key_for(wider, "test") != funes::McpPool::key_for(server("a"), "test"));
    CHECK(funes::McpPool::key_for(server("a"), "other") != funes::McpPool::key_for(server("a"), "test"));
    std::cout << "  ok: a different env, access block or agent gets a different server\n";
    return 0;
}

int test_keep_alive_off_starts_fresh() {
    funes::McpPool pool(false, 600, 2);
    pid_t pid;
    {
        auto l = lease(pool);
        pid = std::stoi(call(l, "pid"));
    }
    CHECK(pool.stats().idle == 0);
    CHECK(!alive(pid));
    auto l = lease(pool);
    CHECK(std::stoi(call(l, "pid")) != pid);
    std::cout << "  ok: FUNES_MCP_KEEP_ALIVE=0 starts and stops a server per run\n";
    return 0;
}

int test_clear_stops_idle_servers() {
    funes::McpPool pool(true, 600, 2);
    pid_t pid;
    { auto l = lease(pool); pid = std::stoi(call(l, "pid")); }
    pool.clear();
    CHECK(pool.stats().idle == 0);
    CHECK(!alive(pid));
    std::cout << "  ok: clear() (agents reloaded, shutdown) stops what is idle\n";
    return 0;
}

} // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);   // as the server does: a write to a dead server is an error, not a death
    fs::create_directories(kBase);
    std::ofstream(kBase / "server.py") << kServer;
    int rc = 0;
    rc |= test_a_returned_server_is_reused();
    rc |= test_concurrent_leases_get_their_own_server();
    rc |= test_a_dead_server_is_not_handed_out();
    rc |= test_a_server_killed_while_idle_is_replaced();
    rc |= test_a_discarded_lease_is_stopped();
    rc |= test_idle_servers_are_reaped();
    rc |= test_configuration_is_part_of_the_key();
    rc |= test_keep_alive_off_starts_fresh();
    rc |= test_clear_stops_idle_servers();
    fs::remove_all(kBase);
    if (rc == 0) std::cout << "mcp_pool: all tests passed\n";
    return rc;
}
