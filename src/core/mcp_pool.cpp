// src/core/mcp_pool.cpp — see mcp_pool.h
#include "mcp_pool.h"
#include "funes_config.h"
#include "mcp_launch.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace funes {

namespace {

using Clock = std::chrono::steady_clock;

struct Idle {
    std::unique_ptr<mcp::stdio_client> client;
    std::vector<mcp::tool> tools;
    std::string label;
    int uses = 0;
    Clock::time_point since;
};

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += s + '\x1f';
    return out;
}

long ms_since(Clock::time_point t) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

} // namespace

struct McpPool::Impl {
    bool keep_alive;
    std::chrono::seconds idle_for;
    size_t idle_max;

    mutable std::mutex mu;
    std::map<std::string, std::deque<Idle>> idle;   // newest at the back
    McpPoolStats stats;

    std::condition_variable cv;
    bool stopping = false;
    std::thread reaper;
};

// ── lease ─────────────────────────────────────────────────────────────────────

McpLease::McpLease(McpLease&& o) noexcept { *this = std::move(o); }

McpLease& McpLease::operator=(McpLease&& o) noexcept {
    if (this == &o) return *this;
    if (pool_ && client_) pool_->release(*this);
    pool_ = o.pool_;          o.pool_ = nullptr;
    key_ = std::move(o.key_);
    label_ = std::move(o.label_);
    client_ = std::move(o.client_);
    tools_ = std::move(o.tools_);
    uses_ = o.uses_;
    discard_ = o.discard_;
    return *this;
}

McpLease::~McpLease() {
    if (pool_ && client_) pool_->release(*this);
}

// ── pool ──────────────────────────────────────────────────────────────────────

McpPool& McpPool::instance() {
    // Never destroyed: a static's destructor would stop servers after main()
    // returned, when what their on_exit reports to (the machine collector) is
    // already gone. main() calls clear() on the way out instead.
    static McpPool* pool = [] {
        const std::string keep = funes::env("FUNES_MCP_KEEP_ALIVE");
        const std::string idle = funes::env("FUNES_MCP_IDLE_SECONDS");
        const std::string max  = funes::env("FUNES_MCP_POOL_IDLE_MAX");
        int idle_s = 600;
        size_t idle_n = 2;
        try { if (!idle.empty()) idle_s = std::max(0, std::stoi(idle)); } catch (...) {}
        try { if (!max.empty())  idle_n = static_cast<size_t>(std::max(0, std::stoi(max))); } catch (...) {}
        return new McpPool(keep != "0", idle_s, idle_n);
    }();
    return *pool;
}

McpPool::McpPool(bool keep_alive, int idle_seconds, size_t idle_max)
    : impl_(std::make_unique<Impl>()) {
    impl_->keep_alive = keep_alive && idle_max > 0;
    impl_->idle_for = std::chrono::seconds(idle_seconds);
    impl_->idle_max = idle_max;
    if (impl_->keep_alive) {
        impl_->reaper = std::thread([this] {
            std::unique_lock<std::mutex> lock(impl_->mu);
            while (!impl_->stopping) {
                impl_->cv.wait_for(lock, std::chrono::seconds(5));
                if (impl_->stopping) break;
                lock.unlock();
                reap();
                lock.lock();
            }
        });
    }
}

McpPool::~McpPool() {
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    if (impl_->reaper.joinable()) impl_->reaper.join();
    clear();
}

std::string McpPool::key_for(const McpServerConfig& srv, const std::string& agent) {
    std::string key = agent + '\x1e' + srv.name + '\x1e' + srv.command + '\x1e' + srv.env.dump() + '\x1e' +
                      join(srv.access.read) + '\x1e' + join(srv.access.exec) + '\x1e' +
                      join(srv.access.write) + '\x1e';
    for (int p : srv.access.connect) key += std::to_string(p) + ',';
    return key;
}

McpLease McpPool::acquire(const McpServerConfig& srv, const std::string& agent,
                          const std::string& client_name, std::string& error) {
    McpLease lease;
    lease.pool_ = this;
    lease.key_ = key_for(srv, agent);
    lease.label_ = "'" + (srv.name.empty() ? srv.command : srv.name) + "' (agent " + agent + ")";

    // An idle server, newest first; one that died while it waited is stopped
    // (outside the lock — the vendored client's stop can take two seconds).
    while (impl_->keep_alive) {
        Idle found;
        {
            std::lock_guard<std::mutex> lock(impl_->mu);
            auto it = impl_->idle.find(lease.key_);
            if (it == impl_->idle.end() || it->second.empty()) break;
            found = std::move(it->second.back());
            it->second.pop_back();
            if (it->second.empty()) impl_->idle.erase(it);
        }
        if (!found.client->is_running()) {
            std::cerr << "[mcp] " << lease.label_ << " had exited while idle — starting another\n";
            continue;   // `found` stops and reaps it here
        }
        lease.client_ = std::move(found.client);
        lease.tools_ = std::move(found.tools);
        lease.uses_ = found.uses + 1;
        {
            std::lock_guard<std::mutex> lock(impl_->mu);
            ++impl_->stats.reused;
        }
        std::cerr << "[mcp] " << lease.label_ << " reused, use " << lease.uses_ << "\n";
        return lease;
    }

    const auto t0 = Clock::now();
    auto client = make_stdio_client(srv, agent);
    try {
        if (!client->initialize(client_name, "1.0.0")) {
            error = "did not initialize";
            lease.pool_ = nullptr;
            return lease;
        }
        lease.tools_ = client->get_tools();
    } catch (const std::exception& e) {
        error = e.what();
        lease.pool_ = nullptr;
        return lease;
    }
    lease.client_ = std::move(client);
    lease.uses_ = 1;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        ++impl_->stats.started;
    }
    std::cerr << "[mcp] " << lease.label_ << " started in " << ms_since(t0) << " ms, "
              << lease.tools_.size() << " tools" << (impl_->keep_alive ? "" : " (keep-alive off)") << "\n";
    return lease;
}

void McpPool::release(McpLease& lease) {
    std::unique_ptr<mcp::stdio_client> client = std::move(lease.client_);
    lease.pool_ = nullptr;
    const char* why = !impl_->keep_alive              ? nullptr
                    : lease.discard_                  ? "a call through it failed"
                    : !client->is_running()           ? "it exited"
                                                      : "";
    if (why && *why == '\0') {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (!impl_->stopping) {
            auto& q = impl_->idle[lease.key_];
            if (q.size() < impl_->idle_max) {
                q.push_back({std::move(client), std::move(lease.tools_), lease.label_, lease.uses_, Clock::now()});
                return;
            }
            why = "the pool already holds enough of it";
        } else {
            why = "the pool is shutting down";
        }
    }
    if (why) std::cerr << "[mcp] " << lease.label_ << " stopped: " << why << "\n";
    client.reset();   // stops the process: SIGTERM, reap, cgroup on_exit
}

void McpPool::reap() {
    std::vector<Idle> expired;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        const auto now = Clock::now();
        for (auto it = impl_->idle.begin(); it != impl_->idle.end();) {
            auto& q = it->second;
            while (!q.empty() && (now - q.front().since >= impl_->idle_for || !q.front().client->is_running())) {
                expired.push_back(std::move(q.front()));
                q.pop_front();
            }
            it = q.empty() ? impl_->idle.erase(it) : std::next(it);
        }
    }
    for (auto& e : expired)
        std::cerr << "[mcp] " << e.label << " stopped: "
                  << (e.client->is_running() ? "idle" : "exited while idle") << " after " << e.uses
                  << (e.uses == 1 ? " use\n" : " uses\n");
}   // `expired` stops them here, outside the lock

void McpPool::clear() {
    std::map<std::string, std::deque<Idle>> all;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        all.swap(impl_->idle);
    }
    size_t n = 0;
    for (auto& [k, q] : all) n += q.size();
    if (n) std::cerr << "[mcp] stopping " << n << " idle server(s)\n";
}

McpPoolStats McpPool::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    McpPoolStats s = impl_->stats;
    for (const auto& [k, q] : impl_->idle) s.idle += q.size();
    return s;
}

} // namespace funes
