// =============================================================================
// src/core/tools/http_tool_runtime.h — runtime for LLM-scaffolded HTTP tools
// =============================================================================
//
// The `create_tool` meta-tool (tool_builder.cpp) doesn't compile anything at
// runtime — it writes a small .cpp file into src/core/tools/generated/ that
// only takes effect after a rebuild ("delivered with each new Funes release").
// Each generated file is a thin, deterministic wrapper: it supplies fixed
// configuration (method, URL template, headers, body template) to the one
// shared handler here. The LLM never contributes executable logic, only data
// — the same trust boundary as a webhook/REST config, not code generation.
//
// Placeholder rules for make_http_template_tool:
//   - `{param}` in url_template is replaced with the percent-encoded value of
//     args["param"].
//   - `{param}` in body_template is replaced with the JSON-escaped value of
//     args["param"], INCLUDING the surrounding quotes for strings — write the
//     template without manual quotes, e.g. {"query": {q}} not {"query": "{q}"}.
//   - `${ENV_VAR}` in header values is replaced with getenv("ENV_VAR") at call
//     time, so a generated file can reference an API key without ever
//     containing the literal secret.
//   - Same private/loopback-host guard as web_fetch (net_guard.h): refused
//     unless FUNES_ALLOW_LOCAL_FETCH=1.
// =============================================================================

#pragma once
#include "../tools.h"
#include <map>
#include <string>

namespace funes::tools {

using RegisterFn = void (*)(ToolRegistry&);

// Called by each generated tool's file-scope static initializer to queue
// itself; register_all_generated_tools() applies the queue once at startup.
// A function-local static vector (not a global) sidesteps static
// initialization order across translation units.
void register_generated(RegisterFn fn);
void register_all_generated_tools(ToolRegistry& reg);

// Response text is "HTTP <status>\n<body>", body capped at 8 KB.
ToolHandler make_http_template_tool(const std::string& method,
                                    const std::string& url_template,
                                    const std::map<std::string, std::string>& headers,
                                    const std::string& body_template);

} // namespace funes::tools
