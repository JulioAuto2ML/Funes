# src/server/

The HTTP server, REST/SSE API, and application entry point. Four files.
Everything runs in a single `funes` binary.

## Files

| File | Purpose |
|---|---|
| `main.cpp` | Entry point. Config loading, service construction, background threads, HTTP server startup. |
| `api.h/cpp` | `FunesApi` -- route handlers, agent table, SSE streaming, authentication. |
| `user_cli.h/cpp` | `funes useradd/userdel/userlist/passwd/perms/jid-map/jid-unmap`. Runs against the database and exits without starting the server, so account management never needs a reachable LLM. |
| `maint_cli.h/cpp` | `funes cron-cleanup`. One-off, destructive database maintenance -- separate from `user_cli` because it operates on stored conversations and memories rather than accounts, and is run once after an upgrade rather than as part of running the box. |

## Startup sequence

1. **Load config** -- layered: shell env > `config/funes.local` > `config/funes.conf` > `~/.funes/config`
2. **Resolve paths** -- data directories, agents directory, workspace, publications.
   A CLI subcommand is dispatched here and exits; the workspace is migrated to
   its per-user layout (`<root>/<user_id>/`) before any tool can look in it
3. **Discover model** -- queries `/v1/models` when model is `default` and provider is OpenAI-compatible
4. **Construct services** -- `MemoryStore` and `UserStore` (same SQLite file, separate connections), `ToolRegistry` (all built-ins), `FunesApi` (routes + agent table)
5. **Wire circular references** -- delegation, agent creation, cron, and roster injection all need the agent table that `FunesApi` owns
6. **Start background threads** (all detached):
   - Embedding backfill (fills missing vectors when endpoint recovers). Drains
     in a loop rather than making one capped call, and retries on a slow timer
     while the embedding endpoint is unreachable -- see the note below
   - Result pruning (sweeps expired tool results)
   - Memory consolidation (merges near-duplicates, prunes stale auto-memories)
   - Cron runner (polls for due scheduled jobs)
7. **Start HTTP server** -- `httplib::Server` with 60s read / 1200s write timeout

## API routes

Everything under `/api/` requires authentication except the ones marked
*public*; the ones marked *admin* additionally refuse a member with 403. That is enforced twice: a pre-routing gate refuses any
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
| `/api/auth/status` | GET | `{needs_bootstrap, authenticated, user?, permissions?}` (*public*) |
| `/api/auth/bootstrap` | POST | Creates the first admin; refused once any user exists (*public*) |
| `/api/status` | GET | Health: model name, memory count, agent count |
| `/api/agents` | GET | List agents (name + description) |
| `/api/agents/reload` | POST | Hot-reload all agent YAMLs |
| `/api/chat` | POST | Main chat -- returns SSE stream |
| `/api/memories` | GET | List or search memories |
| `/api/memories` | POST | Create a memory |
| `/api/memories/<id>` | DELETE | Delete a memory |
| `/api/history` | GET | Get session conversation turns |
| `/api/sessions` | GET | List sessions with previews. `?cron=1` also lists scheduled-run transcripts, hidden by default |
| `/api/sessions/<name>` | DELETE | Delete one conversation (turns, summary, stored results; memories survive) |
| `/api/jobs` | GET | List the caller's scheduled cron jobs. `?all=1` lists every account's, with the owning username (*admin*) |
| `/api/upload` | POST | Upload file to workspace |
| `/*` | GET | Static files (web UI) |

### What `/api/auth/status` reports about permissions

Identity alone was not enough: a member who found an agent missing or a tool
refused had no way to learn why, and an admin had to SSH in and run
`funes perms <user>` to inspect anyone. The route now also returns the
caller's *resolved* permissions -- allowed agents intersected with the agents
actually loaded, and a deny-list over every registered tool -- rather than
echoing back the raw `users.permissions` blob, whose "absent" means different
things for the two fields. The UI would otherwise have to re-implement that
rule in JavaScript and would eventually disagree with `permissions.cpp`.

Read-only. Editing stays in the CLI. It is always the *caller's* own
permissions: the route is public, so returning anyone else's would describe
the box's roster to an anonymous request. `/api/login` returns the same block
so the UI does not have to re-fetch status on a page it just authenticated.

### The embedding backfill drains

`MemoryStore::backfill_embeddings` is capped per call because it takes the
write lock per row on a server that is already answering requests. One call is
therefore not "the backfill" -- the startup thread loops until a pass reports
nothing done. Before that it ran once, so a database with more than 256
memories missing vectors came back with the remainder on keyword-only recall
until the next restart. It went unnoticed for a long time because only a
migration drops every vector at once; normally there are a handful to fill.

A pass returning 0 is ambiguous -- nothing left, or the endpoint is down, since
`backfill_embeddings` stops at the first failed embed rather than burning
through the backlog against a dead endpoint. `count_missing_vectors()`
separates the two, so an embedding endpoint that comes up *after* Funes does
(the ordinary case when both start at boot) gets retried rather than waiting
for a restart.

### Scheduled runs are hidden, not discarded

Every cron firing gets its own session (`cron-<id>-<epoch>`, one per run by
design so tool budgets do not carry over). On the deployment those were 18% of
the conversation list -- two-turn transcripts nobody had held. They are still
the only record of what a run older than the last one did, so `/api/sessions`
hides them and `?cron=1` is how you get back to them; the per-run epoch makes
the name unguessable, so hiding without a way to list would be losing them.

The auto-memory those runs used to write is a separate matter and is simply
gone -- see `src/core/README.md`. `funes cron-cleanup` removes the ones an
older database already holds.

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
