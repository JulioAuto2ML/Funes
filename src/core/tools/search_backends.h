#pragma once
// =============================================================================
// src/core/tools/search_backends.h — which engine answers web_search
// =============================================================================
// FUNES_SEARCH_BACKEND picks the first engine tried: "tavily" (the default) or
// "parallel" (parallel_search.h). Tavily is always the last resort, so a
// Parallel outage or rate limit degrades to the old behaviour instead of a
// failed search. Anything else is treated as unset — a typo must not leave an
// install with no search at all.

#include "tavily.h"
#include <functional>
#include <string>
#include <vector>

namespace funes::search {

using SearchFn = std::function<std::string(const tavily::Query&, std::vector<tavily::Hit>&)>;

struct Backend {
    std::string name;
    // Per-hit snippet cut handed to format_hits. Tavily's extract is generic,
    // so a short lead is all it is worth; Parallel's excerpts are selected for
    // the query and are most of what the search is for.
    size_t      snippet_limit;
    SearchFn    fn;
};

// In the order to try them, per FUNES_SEARCH_BACKEND.
std::vector<Backend> configured_backends();

// Try each backend until one succeeds. Empty return = success, with `used` the
// index of the backend that answered. Otherwise every backend's error, named,
// in one message, and `used` is -1. Each fallback is logged to stderr.
std::string run(const std::vector<Backend>& backends, const tavily::Query& q,
                std::vector<tavily::Hit>& out, int& used);

} // namespace funes::search
