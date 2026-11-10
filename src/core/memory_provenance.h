// =============================================================================
// src/core/memory_provenance.h — where a memory came from
// =============================================================================
// docs/memory-plan.md, phase 3. Until 7.1 a memory knew only its `source`
// (user / tool / auto / consolidated), and `tool` meant "the model called
// remember" — whether it was writing down something the person said or
// copying a sentence out of a web page it had just fetched. Both were weighted
// 1.3, the same as something the person taught deliberately (5.0.1 review,
// item #8).
//
// A Provenance travels with each write:
//   origin   where the text came from: "ui", "session:<id>", "url:<host>",
//            "tool:<name>", "script:<name>", several joined by ", "
//   tainted  the run had read third-party content (a web page, a search
//            result, an email, another agent's answer) before this write
//
// A tainted memory is shown as an attribution ("per <origin>: …",
// MemoryStore::Memory::presented), ranked at weight 1.0 whatever its source
// (MemoryStore::weight_of), and is never the anchor of a link expansion.

#pragma once
#include "json.hpp"
#include <memory>
#include <set>
#include <string>

namespace funes {

struct Provenance {
    std::string origin;
    bool        tainted = false;
};

// The third-party content one agent run has read so far. Owned by
// FunesAgent::run, shared through ToolContext so the remember tool can see
// what the loop saw; null in a ToolContext built anywhere else, which reads
// as "nothing third-party seen".
struct RunProvenance {
    std::set<std::string> sources;   // "url:<host>", "tool:<name>", ...

    bool tainted() const { return !sources.empty(); }
    // Up to three sources, joined by ", "; "+N more" past that.
    std::string describe() const;
};

// What reading the result of `tool` called with `args` exposes the run to:
// "" for a built-in that returns only the caller's own data, otherwise the
// origin to record ("url:<host>" for web_fetch, "script:<name>" for
// run_script, "tool:<name>" for anything else).
//
// Deny by default, like scripts: — a tool added tomorrow, an MCP tool, a
// generated HTTP tool all taint until someone writes down that they don't.
std::string third_party_origin(const std::string& tool, const nlohmann::json& args);

// The text a tainted memory is shown as: "per <origin>: <text>".
std::string attributed(const std::string& text, const std::string& origin);

} // namespace funes
