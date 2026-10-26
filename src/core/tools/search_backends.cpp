// =============================================================================
// src/core/tools/search_backends.cpp — see search_backends.h
// =============================================================================

#include "search_backends.h"
#include "parallel_search.h"
#include "../funes_config.h"
#include <iostream>

namespace funes::search {

namespace {
// Parallel's excerpts run to ~900 bytes a hit; 500 keeps the useful part and
// eight hits near 5 KB, inline (web_search is exempt from the result store).
constexpr size_t kParallelSnippetLimit = 500;
} // namespace

std::vector<Backend> configured_backends() {
    const Backend tavily_backend{"tavily", tavily::kSnippetLimit, tavily::search};
    if (funes::env("FUNES_SEARCH_BACKEND") == "parallel")
        return {{"parallel", kParallelSnippetLimit, parallel::search}, tavily_backend};
    return {tavily_backend};
}

std::string run(const std::vector<Backend>& backends, const tavily::Query& q,
                std::vector<tavily::Hit>& out, int& used) {
    used = -1;
    if (backends.empty()) return "web search is not configured: no search backend";
    std::string errors;
    for (size_t i = 0; i < backends.size(); ++i) {
        std::vector<tavily::Hit> hits;
        const std::string err = backends[i].fn(q, hits);
        if (err.empty()) {
            if (i > 0)
                std::cerr << "[web_search] answered by " << backends[i].name
                          << " after: " << errors << "\n";
            used = static_cast<int>(i);
            for (auto& h : hits) out.push_back(std::move(h));
            return {};
        }
        if (!errors.empty()) errors += "; ";
        errors += backends[i].name + ": " + err;
        if (i + 1 < backends.size())
            std::cerr << "[web_search] " << backends[i].name << " failed (" << err
                      << "), falling back to " << backends[i + 1].name << "\n";
    }
    return backends.size() == 1 ? errors.substr(backends[0].name.size() + 2) : errors;
}

} // namespace funes::search
