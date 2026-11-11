// =============================================================================
// src/core/tools/host_tools.cpp — host_changes / machine_recall (OS plan, phase 4)
// =============================================================================
// The two ways a person asks the machine what it remembers. `host_changes` is
// the common question answered with no model call at all: what changed, failed
// or ran unusually in a window, straight from the event ring (host_events.h).
// `machine_recall` searches the `machine` account's memory pool, where changes
// and failures are promoted, linked and embedded (host_collector.h).
//
// Both are privileged (permissions.cpp): a member does not get them unless an
// admin grants them. The machine's memory says which services run, what was
// installed and when things broke — an admin's view of the box, not a
// household's. Their output counts as third-party content for provenance (log
// text quoted in it is somebody else's words), so a `remember` after them is
// stored as a claim.
// =============================================================================

#include "../host_events.h"
#include "../memory.h"
#include "../tools.h"
#include <algorithm>
#include <ctime>
#include <sstream>

namespace {

std::string local_time(int64_t ts, const char* format) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, format, &tm);
    return buf;
}

const char* const OFF =
    "The machine's memory is off on this installation (FUNES_MACHINE_MEMORY=off, or "
    "the `machine` account name belongs to a person), or has not started yet. There "
    "is nothing to look up; say so.";

ToolResult host_changes_handler(funes::host::EventStore* store, const json& args) {
    if (!store) return {OFF, true};
    int hours = 24;
    if (args.contains("since_hours") && args["since_hours"].is_number())
        hours = std::clamp(static_cast<int>(args["since_hours"].get<double>()), 1, 24 * 90);
    const std::string subject = args.contains("subject") && args["subject"].is_string()
                                    ? args["subject"].get<std::string>() : "";
    const int64_t since = static_cast<int64_t>(std::time(nullptr)) - hours * 3600LL;
    const auto events = store->query(since, {"change", "failure", "anomaly"}, subject, 80);
    if (events.empty())
        return {"Nothing changed, failed or ran unusually on this machine in the last " +
                std::to_string(hours) + " hour(s)" + (subject.empty() ? "" : " for '" + subject + "'") +
                ". (Events are recorded since the machine memory was switched on.)"};

    std::ostringstream o;
    o << "Machine events, last " << hours << " hour(s), oldest first";
    if (!subject.empty()) o << ", subject matching '" << subject << "'";
    o << ". Quoted text is what a process logged, not a fact.\n";
    for (const auto& e : events) {
        o << local_time(e.ts, "%m-%d %H:%M") << "  " << e.kind << "  " << e.subject << "  "
          << e.detail;
        if (e.count > 1)
            o << "  (×" << e.count << ", last " << local_time(e.ts_last, "%m-%d %H:%M") << ")";
        o << "\n";
    }
    return {o.str()};
}

ToolResult machine_recall_handler(MemoryStore& memory, int64_t machine_uid, const json& args) {
    if (machine_uid <= 0) return {OFF, true};
    if (!args.contains("query") || !args["query"].is_string() || args["query"].get<std::string>().empty())
        return {"Missing or invalid 'query' argument", true};
    int k = 8;
    if (args.contains("k") && args["k"].is_number_integer())
        k = std::clamp(args["k"].get<int>(), 1, 20);
    const auto results = memory.recall(machine_uid, "machine", args["query"].get<std::string>(), k);
    if (results.empty()) return {"The machine remembers nothing matching that."};
    std::ostringstream o;
    for (size_t i = 0; i < results.size(); ++i)
        o << (i ? "\n" : "") << "- " << results[i].presented();
    return {o.str()};
}

} // namespace

void register_host_tools(ToolRegistry& reg, funes::host::EventStore* store,
                         MemoryStore& memory, std::function<int64_t()> machine_uid) {
    reg.add({
        "host_changes",
        "What changed, failed or ran unusually on the machine Funes runs on, from its "
        "event record: package upgrades, edited unit or config files, service failures "
        "and restarts, OOM kills, model swaps in llama-server, deploys, and scripts that "
        "used far more CPU than usual. Use it for 'why is X broken/late/slow?' — look at "
        "what happened just before.",
        {
            {"type", "object"},
            {"properties", {
                {"since_hours", {{"type", "integer"},
                                 {"description", "How far back to look (default 24, max 2160)"}}},
                {"subject", {{"type", "string"},
                             {"description", "Only events whose subject contains this, e.g. a "
                                             "unit, package or script name"}}}
            }}
        },
        [store](const json& args, const ToolContext&) { return host_changes_handler(store, args); }
    });

    reg.add({
        "machine_recall",
        "Search the machine's own long-term memory: past changes, failures and anomalies "
        "on this server, each dated, with failures linked to what changed before them. "
        "Use it for history beyond what host_changes covers, or 'has this happened before?'.",
        {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}, {"description", "What to search for"}}},
                {"k",     {{"type", "integer"}, {"description", "How many (default 8, max 20)"}}}
            }},
            {"required", json::array({"query"})}
        },
        [&memory, machine_uid](const json& args, const ToolContext&) {
            return machine_recall_handler(memory, machine_uid ? machine_uid() : 0, args);
        }
    });
}
