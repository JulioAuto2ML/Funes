// =============================================================================
// src/core/busy_gate.cpp — background work yields to people
// =============================================================================

#include "busy_gate.h"
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace funes::load {

namespace {
std::atomic<int> g_in_flight{0};
}

int runs_in_flight() { return g_in_flight.load(); }

RunInFlight::RunInFlight()  { ++g_in_flight; }
RunInFlight::~RunInFlight() { --g_in_flight; }

double psi_some_avg10(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("some ", 0) != 0) continue;
        const size_t at = line.find("avg10=");
        if (at == std::string::npos) return -1.0;
        const char* start = line.c_str() + at + 6;
        char* end = nullptr;
        const double v = std::strtod(start, &end);
        return end == start ? -1.0 : v;
    }
    return -1.0;
}

bool is_quiet(const QuietOptions& opt) {
    if (runs_in_flight() > 0) return false;
    if (psi_some_avg10(opt.cpu_path) > opt.max_cpu_pressure) return false;
    if (psi_some_avg10(opt.io_path)  > opt.max_io_pressure)  return false;
    return true;
}

bool wait_until_quiet(const QuietOptions& opt) {
    const auto deadline = std::chrono::steady_clock::now() + opt.max_wait;
    while (!is_quiet(opt)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(opt.poll);
    }
    return true;
}

} // namespace funes::load
