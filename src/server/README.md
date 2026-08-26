# src/server/

The HTTP server, REST/SSE API, and application entry point. Three files, ~816
lines. Everything runs in a single `funes` binary.

## Files

| File | Purpose |
|---|---|
| `main.cpp` | Entry point. Config loading, service construction, background threads, HTTP server startup. |
| `api.h/cpp` | `FunesApi` -- route handlers, agent table, SSE streaming, authentication. |
| `user_cli.h/cpp` | `funes useradd/userdel/userlist/passwd/jid-map/jid-unmap`. Runs against the database and exits without starting the server, so account management never needs a reachable LLM. |

## Startup sequence

1. **Load config** -- layered: shell env > `config/funes.local` > `config/funes.conf` > `~/.funes/config`
2. **Resolve paths** -- data directories, agents directory, workspace, publications.
   A CLI subcommand is dispatched here and exits; the workspace is migrated to
   its per-user layout (`<root>/<user_id>/`) before any tool can look in it
3. **Discover model** -- queries `/v1/models` when model is `default` and provider is OpenAI-compatible
4. **Construct services** -- `MemoryStore` and `UserStore` (same SQLite file, separate connections), `ToolRegistry` (all built-ins), `FunesApi` (routes + agent table)
5. **Wire circular references** -- delegation, agent creation, cron, and roster injection all need the agent table that `FunesApi` owns
6. **Start background threads** (all detached):
   - Embedding backfill (fills missing vectors when endpoint recovers)
   - Result pruning (sweeps expired tool results)
   - Memory consolidation (merges near-duplicates, prunes stale auto-memories)
   - Cron runner (polls for due scheduled jobs)
7. **Start HTTP server** -- `httplib::Server` with 60s read / 1200s write timeout

## API routes

Everything under `/api/` requires authentication except the three marked
*public*. That is enforced twice: a pre-routing gate refuses any
unauthenticated `/api/` path, so a route added later is protected by default,
and each handler separately resolves the caller with `require_auth` to scope
what it returns. The gate is the security boundary; the per-handler lookup is
what makes the answer correct.

Callers authenticate with either a session cookie (the web UI) or
`X-Funes-Service-Token` plus `X-Funes-User-Jid` (the WhatsApp autoresponder --
the token proves the caller is trusted, the jid says who the request is for,
and neither alone authenticates anything).

| Route | Method | Purpose |
|---|---|---|
| `/api/login` | POST | Username + password, sets the session cookie (*public*) |
| `/api/logout` | POST | Revokes the token server-side and clears the cookie |
| `/api/auth/status` | GET | `{needs_bootstrap, authenticated, user?}` (*public*) |
| `/api/auth/bootstrap` | POST | Creates the first admin; refused once any user exists (*public*) |
| `/api/status` | GET | Health: model name, memory count, agent count |
| `/api/agents` | GET | List agents (name + description) |
| `/api/agents/reload` | POST | Hot-reload all agent YAMLs |
| `/api/chat` | POST | Main chat -- returns SSE stream |
| `/api/memories` | GET | List or search memories |
| `/api/memories` | POST | Create a memory |
| `/api/memories/<id>` | DELETE | Delete a memory |
| `/api/history` | GET | Get session conversation turns |
| `/api/sessions` | GET | List sessions with previews |
| `/api/jobs` | GET | List scheduled cron jobs |
| `/api/upload` | POST | Upload file to workspace |
| `/*` | GET | Static files (web UI) |

## Chat endpoint

The chat endpoint creates a `FunesAgent` and runs the full agent loop inside a
chunked SSE response. Events streamed during a chat:

| Event | Data |
|---|---|
| `memories` | Recalled facts injected into context |
| `delta` | Streaming text fragment |
| `tool_call` | Tool name + arguments |
| `tool_result` | Tool output (or error) |
| `result_stored` | Large result stored by reference |
| `contract_nudge` | Completion contract reminder |
| `schema_nudge` | Answer schema correction |
| `context_compressed` | Turns folded into summary |
| `usage` | Token count + context limit |
| `done` | Final answer text |
| `error` | Error message |

## Limits

- Session cookie: HttpOnly, SameSite=Strict, 30 days; `Secure` only when
  `FUNES_COOKIE_SECURE=1`, since the default deployment is plain HTTP on a LAN
  and an unconditional `Secure` would make login silently fail there
- Max message size: 16 KB
- Max upload: 5 MB
- Max images per message: 4
- Write timeout: 1200s (for slow local LLMs streaming long responses)
- `SIGPIPE` ignored so dropped SSE clients don't crash the server
