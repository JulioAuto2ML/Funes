// =============================================================================
// src/core/sd_notify.h — systemd readiness and watchdog, without libsystemd
// =============================================================================
// Type=simple told systemd "started" the moment the process existed, and
// nothing after that: a server whose every worker was stuck behind a hung
// LLM call looked exactly as healthy as one answering in milliseconds. With
// Type=notify + WatchdogSec=, Funes says READY=1 once it is accepting
// connections and keeps saying WATCHDOG=1 only while a request to itself gets
// an answer — so "stuck" becomes a restart instead of a silent outage.
//
// The protocol is one datagram to $NOTIFY_SOCKET, so this is fifty lines of
// socket code rather than a dependency. Outside systemd every call is a no-op.

#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace funes::sd {

// Sends `state` ("READY=1", "WATCHDOG=1", "STATUS=...") to $NOTIFY_SOCKET.
// False when there is no socket to send to or the send failed.
bool notify(const std::string& state);

// The ping interval systemd asked for: half of $WATCHDOG_USEC, zero when no
// watchdog is configured or $WATCHDOG_PID names another process.
std::chrono::microseconds watchdog_interval();

// Pings WATCHDOG=1 every `interval` while `probe()` returns true, and withholds
// the ping (logging why) while it returns false — systemd then restarts the
// unit when WatchdogSec runs out. Stops on destruction.
class Watchdog {
public:
    Watchdog(std::chrono::microseconds interval, std::function<bool()> probe);
    ~Watchdog();
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    stop_ = false;
    std::thread             thread_;
};

} // namespace funes::sd
