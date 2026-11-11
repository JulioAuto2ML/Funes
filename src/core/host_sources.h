// =============================================================================
// src/core/host_sources.h — what the machine collector reads (OS plan, phase 4)
// =============================================================================
// One function per source, each either a pure parser (text in, events out) or
// a reader over a path the caller can point at a fixture. The collector
// (host_collector.h) owns the state between ticks; nothing here remembers.
//
// All of it runs unprivileged on yoda: the *user* journal (julio is not in
// `systemd-journal`, so system units are out of reach — an admin decision,
// not code), /var/log/dpkg.log, /proc, /sys, the llama-server pid files
// ~/launch_llama.sh writes, and file metadata.
//
// Log text is hostile input — any process can write to the journal, and an
// SSH login attempt puts an attacker-chosen username in it. So an event is
// built from *fields*; where a message is kept at all it is quoted, capped,
// digit-normalised (so repeats fold) and marks the event tainted.
// =============================================================================

#pragma once
#include "host_events.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace funes::host {

// ── journal ──────────────────────────────────────────────────────────────────

// `journalctl --user -o json` filtered at the source to unit lifecycle
// messages and priority ≤ 3, after `cursor` (or the last `first_window`
// when there is none yet).
std::vector<std::string> journal_argv(const std::string& cursor,
                                      const std::string& first_window = "-10min");

struct JournalBatch {
    std::vector<Event> events;
    std::string        cursor;   // of the last entry read; empty if none
};
JournalBatch parse_journal(const std::string& json_lines);

// ── dpkg ─────────────────────────────────────────────────────────────────────

// install / upgrade / remove / purge lines; status lines are dpkg's
// bookkeeping for the same act and are skipped.
std::vector<Event> parse_dpkg(const std::string& text);

// ── /proc and /sys ───────────────────────────────────────────────────────────

// The `some avg60` of a /proc/pressure/* file.
std::optional<double> psi_some_avg60(const std::string& text);

// /proc/vmstat's oom_kill counter.
std::optional<int64_t> vmstat_oom_kills(const std::string& text);

struct Reading {
    std::string subject;
    double      value = 0;
    double      limit = 0;   // the hardware's own critical point, 0 if none
};

// Every hwmon temperature: "temp:<chip>/<label>" in °C, with temp*_crit.
std::vector<Reading> read_temperatures(const std::string& sys_root = "/sys");

// VRAM used, percent, per card: "gpu:<card>/vram".
std::vector<Reading> read_vram(const std::string& sys_root = "/sys");

// Percent used of each mount point: "disk:<path>".
std::vector<Reading> read_disks(const std::vector<std::string>& paths);

// ── processes the box runs outside systemd ──────────────────────────────────

// ~/launch_llama.sh writes /tmp/llama_pids/llama_<name>.pid. Name → state
// "<pid>|<model path>|<alive 0/1>"; a missing pid file is a missing key.
std::map<std::string, std::string> read_llama(const std::string& pid_dir,
                                              const std::string& proc_root = "/proc");

// ── files ───────────────────────────────────────────────────────────────────

// path → "<mtime>|<size>" for each existing file; a directory contributes its
// regular files (not recursive). Metadata only: funes.local can be watched
// without being read.
std::map<std::string, std::string> fingerprint(const std::vector<std::string>& paths);

// ── helpers shared with the collector ────────────────────────────────────────

// Quoted, digits folded to '#' (so a repeat with a new pid is the same row —
// off for text that does not vary, like a unit's executable path), capped at
// `max` bytes on a UTF-8 boundary.
std::string quote_log_text(const std::string& text, size_t max = 160, bool fold_digits = true);

// Read a whole small file; nullopt if it cannot be opened.
std::optional<std::string> read_text(const std::string& path, size_t max = 1 << 20);

} // namespace funes::host
