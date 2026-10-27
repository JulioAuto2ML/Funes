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

Both directories are built by CMakeLists.txt as part of the main build.

## MCP servers (not compiled in)

The servers the WhatsApp and Gmail agents spawn. They spent 5.1–5.2 in the
`funes-julio` repository and came back with it.

| Directory | What |
|---|---|
| `imap-email-mcp-patched/` | Node.js IMAP/SMTP MCP server, patched: the published npm package never sets the TLS SNI `servername`, so Gmail's IMAP frontend returns a self-signed fallback cert. The patch is in `patches/`; `npm install` there applies it. |
| `whatsapp-mcp/` | Go + Python WhatsApp Web bridge + MCP tools, a patched copy of [lharries/whatsapp-mcp](https://github.com/lharries/whatsapp-mcp). REST API port moved from 8080 to 8090 to avoid llama-server. The bridge binary (`go build -o whatsapp-bridge main.go`, gitignored) runs from its directory with `WHATSAPP_STORE_DIR` pointing into `data/whatsapp/`; the tracked `store` symlink beside it points the Python server at the personal number's store. |

Both run as child processes of the agents' `mcp_servers:` entries (or, for the
bridges, as systemd units). A spawned server gets an allowlisted environment,
so the credentials they need go in the agent YAML's `env:` block, not in the
server's environment.
