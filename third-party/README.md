# third-party/

Vendored dependencies. These are committed to the repo so the build is
self-contained -- no package manager, no network fetch during `cmake`.

## Libraries

| Directory | What | Why vendored |
|---|---|---|
| `sqlite/` | SQLite amalgamation + [sqlite-vec](https://github.com/asg017/sqlite-vec) | Built as a static library with `SQLITE_CORE` so the vector extension is compiled in, not loaded at runtime. |
| `cpp-mcp/` | C++ MCP client library | Provides `httplib.h` (HTTP server + client), `json.hpp` (nlohmann/json), and the MCP SSE/stdio client used to connect to external tool servers. |

`cpp-mcp/` carries one Funes patch beyond upstream: `stdio_client::
set_environment_filter`, the hook through which the core hands a spawned MCP
server an allowlisted environment instead of the process's own (see
`src/core/tools/process_runner.h`).

## Build integration

Both directories are built by CMakeLists.txt as part of the main build. The
MCP *servers* that used to be vendored here (a patched IMAP/SMTP server and a
WhatsApp bridge) moved to the `funes-julio` extension repository with the
agents that use them.
