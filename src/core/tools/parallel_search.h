#pragma once
// =============================================================================
// src/core/tools/parallel_search.h — Parallel's search MCP as a web_search backend
// =============================================================================
// search.parallel.ai/mcp is a free, keyless streamable-HTTP MCP server whose
// web_search returns ranked URLs with excerpts chosen for the query — denser
// than Tavily's generic extract. It is reached here as a *backend* of the
// built-in web_search rather than listed as an agent's MCP server, because its
// tools are named web_search and web_fetch: a native tool wins a name clash
// and the MCP one is skipped (agent.cpp), and an unwrapped 12 KB reply would go
// to the result store and be paged 2 KB at a time — the 2026-10-26 incident.
// Through web_search it gets format_hits' bound, the social-domain exclusion
// and Tavily as the fallback (search_backends.h).
//
// Config: FUNES_PARALLEL_URL (default below), FUNES_PARALLEL_API_KEY (optional,
// sent as a bearer token for higher rate limits).

#include "tavily.h"
#include <string>
#include <vector>

namespace funes::parallel {

constexpr const char* kDefaultUrl = "https://search.parallel.ai/mcp";

// Parse the text of a web_search tool result into Hits. Excerpts are joined
// with " … " and whitespace collapsed (format_hits puts the snippet on one
// line between title and URL); hits without a title or URL, or on an excluded
// domain (the host or any subdomain of it), are dropped. Empty return =
// success; an empty `results` array is not an error.
std::string parse_results(const std::string& text,
                          const std::vector<std::string>& exclude_domains,
                          std::vector<tavily::Hit>& out);

// One search: initialize a session, call web_search with the query as both
// objective and search query, parse. `topic` has no equivalent and is ignored;
// `time_range` is folded into the objective. include_domains becomes site:
// queries. At most q.max_results hits. Errors come back as a message, never
// thrown, worded like tavily::search's.
std::string search(const tavily::Query& q, std::vector<tavily::Hit>& out);

} // namespace funes::parallel
