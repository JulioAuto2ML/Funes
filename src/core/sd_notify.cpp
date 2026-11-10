// =============================================================================
// src/core/sd_notify.cpp — systemd readiness and watchdog, without libsystemd
// =============================================================================

#include "sd_notify.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace funes::sd {

bool notify(const std::string& state) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (!path || !*path) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const size_t len = std::strlen(path);
    if (len >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path, len);
    // "@name" is an abstract-namespace socket: the leading byte is NUL and the
    // address length counts only the bytes actually used.
    if (path[0] == '@') addr.sun_path[0] = '\0';
    const socklen_t addr_len =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);

    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const ssize_t sent = sendto(fd, state.data(), state.size(), MSG_NOSIGNAL,
                                reinterpret_cast<const sockaddr*>(&addr), addr_len);
    close(fd);
    return sent == static_cast<ssize_t>(state.size());
}

std::chrono::microseconds watchdog_interval() {
    const char* usec = std::getenv("WATCHDOG_USEC");
    if (!usec || !*usec) return std::chrono::microseconds(0);
    if (const char* pid = std::getenv("WATCHDOG_PID"); pid && *pid
        && std::strtol(pid, nullptr, 10) != static_cast<long>(getpid()))
        return std::chrono::microseconds(0);
    const long long v = std::strtoll(usec, nullptr, 10);
    return std::chrono::microseconds(v > 0 ? v / 2 : 0);
}

Watchdog::Watchdog(std::chrono::microseconds interval, std::function<bool()> probe) {
    if (interval.count() <= 0) return;
    thread_ = std::thread([this, interval, probe = std::move(probe)] {
        bool was_failing = false;
        std::unique_lock<std::mutex> lk(mu_);
        while (!cv_.wait_for(lk, interval, [this] { return stop_; })) {
            lk.unlock();
            const bool alive = probe();
            if (alive) notify("WATCHDOG=1");
            // Logged on the transition only: one line when it goes bad, one
            // when it recovers, not one per interval.
            if (alive == was_failing)
                std::cerr << (alive ? "[watchdog] self-probe answered again\n"
                                    : "[watchdog] self-probe got no answer — withholding "
                                      "the ping; systemd restarts the unit if this lasts\n");
            was_failing = !alive;
            lk.lock();
        }
    });
}

Watchdog::~Watchdog() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

} // namespace funes::sd
