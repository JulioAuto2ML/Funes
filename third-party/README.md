# third-party/

Vendored dependencies. These are committed to the repo so the build is
self-contained -- no package manager, no network fetch during `cmake`.

## Libraries

| Directory | What | Why vendored |
|---|---|---|
| `sqlite/` | SQLite amalgamation + [sqlite-vec](https://github.com/asg017/sqlite-vec) | Built as a static library with `SQLITE_CORE` so the vector extension is compiled in, not loaded at runtime. |
| `cpp-mcp/` | C++ MCP client library | Provides `httplib.h` (HTTP server + client), `json.hpp` (nlohmann/json), and the MCP SSE/stdio client used to connect to external tool servers. |
| `imap-email-mcp-patched/` | Node.js IMAP/SMTP MCP server (patched) | The published npm package has a TLS SNI bug: node-imap never sets `servername`, causing Gmail's IMAP frontend to return a self-signed fallback cert. The patch is in `patches/`. |
| `whatsapp-mcp/` | Go + Python WhatsApp Web bridge + MCP tools | Vendored + patched copy of [lharries/whatsapp-mcp](https://github.com/lharries/whatsapp-mcp). REST API port moved from 8080 to 8090 to avoid conflict with llama-server. |

## Build integration

`sqlite/` and `cpp-mcp/` are built by CMakeLists.txt as part of the main
build. The Node.js and Go/Python servers are run as child processes by their
respective agents' MCP configurations -- they are not compiled into the binary.
