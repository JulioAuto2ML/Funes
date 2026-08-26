# Funes 4.0 — Users & Permissions

Users and permissions for households and small teams.

> **Status (2026-08-26, branch `feat/v4-users-permissions`).**
> Phases 1–3 are implemented, tested and committed. Phase 5's login and
> first-run screens were pulled forward to sit with phase 1, because phase 1
> on its own left every route behind authentication with no way to
> authenticate from the browser. Phase 4 (permissions) is the remaining
> functional work; phase 5's deferred admin panel is still deferred.
>
> Corrections made while building, kept here so the plan doesn't mislead
> anyone reading it later:
>
> - **`vec_memories` did need changing** — see the schema table below. The
>   original claim that it "joins on `memory_id`, which is already filtered"
>   was wrong.
> - **`forget()` was missing from the list of methods gaining `user_id`**, and
>   it is reachable as `DELETE /api/memories/<id>` with sequential integer
>   ids. So was `get_result()`, isolated only by a client-supplied session
>   string. Both are now owner-scoped.
> - **Cron jobs had no identity to run as.** `cron_runner` builds its agent in
>   a background thread with no request. Jobs record an owner and the runner
>   adopts it.
> - **Turning auth on breaks the WhatsApp autoresponder**, which posts to
>   `/api/chat` with no credentials and logs-and-continues on failure. It
>   needed a service-token path, which this plan did not have.
> - **Password hashing:** PBKDF2-HMAC-SHA256 via OpenSSL rather than a
>   vendored libbcrypt — OpenSSL is already a hard dependency via httplib, so
>   this cost no new one.

---

## Interaction channels

Users interact with Funes through two front doors — the **web UI** and
**WhatsApp**. The HTTP API is internal plumbing (only the web UI and the
WhatsApp autoresponder call it), so it doesn't need a public redesign. User
identity is resolved at the gateway level:

- **Web UI** — login screen sets an httpOnly cookie. The API reads the cookie,
  resolves `user_id`, threads it through. Endpoints stay the same externally.
- **WhatsApp** — the autoresponder already identifies senders by `chat_jid`
  (via the whitelist in `funes.local`). A new `jid → user_id` mapping turns
  that into a Funes user. The WhatsApp number *is* the authentication — a user
  can only interact with Funes from the number the admin registered for them.
  Unrecognized jids get the same silent ignore they get today.

User management (create/edit/delete accounts) is admin-only via CLI — no
API-level user CRUD endpoints in v1.

---

## What changes

### Today (single-user)

- No authentication on any endpoint
- One shared SQLite DB, no `user_id` anywhere
- Memories scoped by agent name only
- Sessions are unowned client UUIDs
- One workspace directory for all file tools
- Credentials (Gmail, WhatsApp, Tavily) are global
- Cron jobs belong to nobody
- Shell access is all-or-nothing

### 4.0 (household / small team)

- Web: cookie auth; WhatsApp: jid → user mapping
- `user_id` on memories, turns, sessions, cron
- Memories isolated per user *and* per agent
- Sessions owned by the authenticated user
- Per-user sandboxed workspace dirs
- Credentials stay admin-managed & shared
- Cron jobs owned by creating user
- Per-user tool permission toggles

---

## Phased plan

Each phase ships independently. Phase 1 is the prerequisite; phases 2–3 can
overlap; phase 4 needs 2; phase 5 needs 1–4.

### Phase 1: Authentication & users table

*Prerequisite for everything else.*

- New `users` table: id, username, display_name, password_hash, role,
  created_at
- Passwords hashed with bcrypt (or argon2 via a small C lib — we already link
  sqlite-vec)
- New `auth_tokens` table: token → user_id, created_at, expires_at
- Auth middleware on `/api/*` routes: read cookie, resolve user_id, inject into
  request context
- Login endpoint: `POST /api/login` → validates credentials, sets httpOnly
  cookie
- First-run bootstrap: if no users exist, prompt the UI to create the admin
  account (no default password)
- WhatsApp: new `jid_users` table mapping `chat_jid → user_id`, managed by the
  existing `whatsapp_whitelist.py` CLI (extended with a `--user` flag)
- Admin manages users via CLI (`funes useradd` / `funes userdel`) — no API-level
  user CRUD in v1

> **No self-registration.** This is a household appliance, not a SaaS. Admin
> creates accounts, hands out credentials. Keeps the auth surface tiny.

### Phase 2: Data isolation

*The big schema migration — every table gains `user_id`.*

- `memories`: add `user_id INTEGER NOT NULL`, change unique constraint to
  `(user_id, agent, text)` (requires recreating the table)
- `turns`: add `user_id`, index changes to `(user_id, session, id)`
- `session_summaries`: add `user_id` to PK
- `tool_results`: inherits isolation via session ownership (add `user_id` for
  direct queries)
- `cron_jobs`: add `user_id`, only owner and admin can see/manage
- `vec_memories`: no change needed — it joins on `memory_id`, which is already
  filtered

#### Migration strategy

Create user 1 (the admin/you) during migration. All existing rows get
`user_id = 1`. The `migrate()` function in `memory.cpp` already handles
incremental schema changes via `add_column_if_missing()` — same pattern, just
more columns.

#### Code changes

- `ToolContext` gains a `user_id` field (threaded from the authenticated
  request)
- `MemoryStore` methods gain a `user_id` parameter: `remember()`, `recall()`,
  `list()`, `count()`, `append_turn()`, `recent_turns()`, `list_sessions()`,
  and — missing from the original list, and the two that were security bugs —
  `forget()` and `get_result()`, plus `turn_count()`, `prune_turns()`,
  `get_summary()`, `set_summary()`, `store_result()`, `prune_results()` and
  the cron accessors. No default value on the parameter: a default makes
  "forgot to pass it" a silent misattribution instead of a compile error
- Every SQL query adds `WHERE user_id = ?`
- `FunesAgent::run()` passes user_id through to memory and tool calls

> **Memory isolation is the critical invariant.** A recall for user A must never
> surface user B's memories. This is enforced at the SQL level (WHERE clause),
> not at the API level — defense in depth.

### Phase 3: Workspace isolation

*Per-user file sandboxes.*

- Workspace root becomes `data/workspaces/<user_id>/`
- `read_file` / `write_file` / `execute_shell` resolve paths against the user's
  workspace, not the global one
- The existing `fs_guard` (path traversal protection) already validates paths
  stay inside the workspace root — just needs the root to be per-user
- `AgentConfig::workspace_dir` overrides still work, but are relative to the
  user's root
- `/api/upload` saves to the authenticated user's workspace
- Migration: move existing workspace contents into user 1's directory

### Phase 4: Permissions

*What each user can do.*

Not a full RBAC system — two roles plus per-user tool toggles.

#### Roles

- **admin** — full access: manage users, all tools, all agents, all cron jobs,
  server config
- **member** — chat with allowed agents and tools, own workspace, own memories,
  own cron jobs

#### Per-user tool permissions (stored in `users` table as JSON)

```json
{
  "tools": {
    "execute_shell": false,
    "create_agent": false,
    "create_tool": false,
    "delegate_to_agent": true,
    "web_search": true,
    "web_fetch": true,
    "read_file": true,
    "write_file": true
  },
  "agents": ["funes", "researcher"]
}
```

- Agent allowlist: which agents this user can talk to or delegate to (empty =
  all)
- Tool allowlist: intersected with the agent's own tool list at runtime
- Default permissions for new members: chat + web search + files, no
  shell/agent-creation

### Phase 5: UI

*Login, user indicator.*

- Login screen (username + password, no OAuth)
- First-run setup screen (create admin account)
- User indicator in the header (who am I, logout)
- Session list filtered to current user (already the API behavior after phase 2)

Admin panel (user list, permissions editor) is deferred — v1 manages users via
CLI. The UI only needs login/logout and the user indicator.

---

## Schema changes

| Table | Change | Details |
|-------|--------|---------|
| **users** | New | `id INTEGER PK, username TEXT UNIQUE, display_name TEXT, password_hash TEXT, role TEXT, permissions TEXT, created_at TEXT` |
| **auth_tokens** | New | `token TEXT PK, user_id INTEGER FK, created_at TEXT, expires_at TEXT` |
| **jid_users** | New | `chat_jid TEXT PK, user_id INTEGER FK` — maps WhatsApp senders to Funes users |
| memories | Add column | `user_id INTEGER NOT NULL DEFAULT 1` — unique constraint becomes `(user_id, agent, text)` |
| turns | Add column | `user_id INTEGER NOT NULL DEFAULT 1` — index becomes `(user_id, session, id)` |
| session_summaries | Add column | `user_id INTEGER NOT NULL DEFAULT 1` |
| tool_results | Add column | `user_id INTEGER NOT NULL DEFAULT 1` |
| cron_jobs | Add column | `user_id INTEGER NOT NULL DEFAULT 1` |
| vec_memories | **Rebuilt** | Adds `user_id INTEGER PARTITION KEY`. The original "no change needed" was wrong: `recall_semantic` ran KNN across every row and filtered afterwards in C++ (vec0 refuses to combine `MATCH` with an arbitrary `WHERE`). Under multi-user the shared `k*8` candidate pool means a busy account crowds a quiet one out of its own top-k — not a leak, but recall quality decaying as accounts are added. A partition key is the one predicate vec0 *will* combine with `MATCH`, and it prunes partitions rather than post-filtering |
| memories | Rebuilt | Also needs the unique constraint changed from `(agent, text)` to `(user_id, agent, text)` — SQLite cannot alter a constraint in place, so the table is recreated in one transaction. Without this the second user to store a fact silently gets the first user's row id back and stores nothing |
| meta | No change | Server-level key/value store, not user-scoped |

---

## API changes (internal, not a public contract)

The HTTP API is only called by the web UI and the WhatsApp autoresponder —
both under our control. The external contract doesn't change. Internally:

- **Two new endpoints:** `POST /api/login` (set cookie), `POST /api/logout`
  (clear cookie). Minimal surface.
- **Auth middleware:** every existing `/api/*` handler resolves `user_id` from
  the cookie before doing anything else. The handler signatures don't change —
  user_id is injected into the request context, not the URL.
- **Existing endpoints unchanged externally:** `/api/chat`, `/api/memories`,
  `/api/sessions`, `/api/history`, `/api/jobs`, `/api/upload` all keep their
  request/response shapes. The only difference is that each one now filters by
  the authenticated user internally.
- **No user CRUD API:** user management is CLI-only in v1 (`funes useradd`).
  An admin panel can come later without touching the API.

---

## Key code paths affected

The request pipeline (where user_id threads through):

```
HTTP request
  → auth middleware (extract user_id from cookie/token)
  → api.cpp handler (passes user_id to...)
  → FunesAgent::run(message, session, user_id, ...)
     → ToolContext{agent, session, workspace, memory_scope, user_id}
     → memory_.recall(agent, query, k, touch, user_id)
     → memory_.remember(agent, text, source, user_id)
     → memory_.append_turn(session, agent, role, content, user_id)
     → file tools resolve workspace as data/workspaces/user_id/
```

---

## LLM concurrency under multi-user

This is the infrastructure constraint that shapes everything. Today
llama-server runs with one slot — requests are serialized. Two family members
chatting at the same time means one waits.

### How llama-server handles concurrent requests

llama-server uses a **slot system**. Each slot is an independent context window
that can serve one request at a time. The `--parallel N` (or `-np N`) flag sets
how many slots are available.

- **Default (`-np 1`):** One slot. Requests queue internally — the second user
  waits until the first response is fully generated. No 503, just latency.
- **`-np N`:** N slots process in parallel via continuous batching. The GPU
  processes tokens from multiple slots in the same batch, so 4 concurrent users
  aren't 4x slower — they share GPU cycles efficiently.
- **Context is split:** Total `-c` is divided among slots.
  `-c 32768 -np 4` = 8192 tokens per slot.

### The lucky alignment

Funes already caps every agent at `context_limit = 8192` tokens (the default in
`agent_config.h`). That means the LLM never sees more than ~8K tokens per
request, regardless of how long the conversation is — older turns get compressed
or pruned by Funes itself.

So the right llama-server config for 4 concurrent users is:

```bash
llama-server -m qwen3.5-9b.gguf -c 32768 -np 4 -fa
```

32K total context / 4 slots = 8K per slot — exactly what Funes sends. No wasted
context, no truncation.

### VRAM budget on yoda

Current setup: Qwen3.5-9B Q8 with 100K context uses ~14.6GB of 16GB VRAM. The
KV cache scales linearly with context size.

| Config | Total context | Slots | Per-slot | KV cache vs today |
|--------|--------------|-------|----------|-------------------|
| Today | 100K | 1 | 100K | 1.0x (baseline) |
| **Recommended** | 32K | 4 | 8K | ~0.32x |
| Conservative | 16K | 2 | 8K | ~0.16x |
| Maximum | 49K | 6 | ~8K | ~0.49x |

Dropping from 100K to 32K context **frees VRAM** — the KV cache shrinks to
roughly a third. We were over-provisioning context that Funes never uses. The
freed memory easily accommodates 4 parallel slots.

### What Funes needs to handle

- **Queue signaling:** When all slots are busy, llama-server queues the request
  (it doesn't 503). Funes should emit a new SSE event — `event: queued` — so
  the UI can show "waiting for model..." instead of an indefinite spinner.
- **Slot health in `/api/status`:** Surface the model's slot usage
  (busy/available) via llama-server's `GET /health` endpoint, which returns
  `{"slots_idle": N, "slots_processing": M}`.
- **Embedding server is fine:** The embedding model on port 8081 already handles
  concurrent requests well — embeddings are stateless and fast.
- **Cron jobs compete for slots:** A scheduled agent run (newsletter pipeline)
  occupies a slot for its entire multi-step run. During that time, one fewer
  slot is available for interactive users. Worth considering a "low-priority"
  slot concept, or just documenting that cron jobs should be scheduled for
  off-peak hours.

> **Net effect:** Multi-user actually *improves* resource efficiency. The
> current 100K context is massively over-provisioned for Funes's 8K agent limit.
> Splitting it into 4 parallel slots uses less VRAM total while serving 4 users
> concurrently.

### Hardware is deployment-specific, not a Funes limitation

The numbers above reflect **yoda** — a home server with 16GB VRAM running a 9B
model. That's the household baseline, not the ceiling. On better hardware the
picture changes completely:

- **48GB+ VRAM** (e.g. RTX A6000, dual consumer GPUs): run a 70B model with 8+
  parallel slots at 8K each, comfortably serving a 10–20 person team.
- **Multi-GPU or cloud instances:** llama-server supports tensor parallelism
  across GPUs. A small company with two A100s could run a 70B model with 16+
  slots.
- **Larger context per slot:** if agents need longer conversations (raising
  `context_limit` beyond 8K), more VRAM just means larger slots — the
  `-c` / `-np` ratio is the only knob.

Funes itself imposes no hardware requirements beyond "an LLM endpoint that
accepts concurrent requests." The `-np` tuning is a deployment decision, not an
application one — document it in the admin guide, not the code.

---

## What stays the same

- **Credentials are global.** One Gmail account, one WhatsApp bridge, one
  Tavily key — managed by admin in `funes.local`. No per-user credential vault
  (that's SaaS territory).
- **Agent definitions are global.** `agents/*.yaml` files are shared.
  Permissions control *who can use* an agent, not per-user agent configs.
- **One LLM backend.** Everyone shares the same llama-server with `-np` slots.
  No per-user model selection or quotas — the slot pool is first-come-first-served.
- **One SQLite file.** Isolation is row-level (`user_id` in WHERE clauses), not
  database-level. Fine for household scale.
- **The agent runtime.** `FunesAgent`, tool dispatch, completion contracts, tool
  budgets, answer schemas — all unchanged in structure, just threaded with
  user_id.
- **Publishing pipeline.** Newsletter/curator tools stay admin-only by default
  via permissions.

---

## Open decisions

- ~~**Password hashing library?**~~ **Decided: PBKDF2-HMAC-SHA256 via
  OpenSSL.** OpenSSL is already required (httplib links it for HTTPS), so this
  cost no new dependency, where vendoring libbcrypt would have. Weaker than
  bcrypt/argon2 against offline GPU cracking, which is the right trade for a
  threat model that is mostly "someone has a copy of memory.db". The stored
  form is self-describing (`pbkdf2_sha256$iterations$salt$hash`) so the cost
  can be raised later without invalidating existing hashes.

- **Token format?** Recommendation: Random 256-bit hex strings stored in the
  DB. Simpler than JWT, and we already hit the DB every request anyway (SQLite
  is local). No token-revocation problem.

- **WhatsApp per-user or shared?** Recommendation: Shared for now. The WhatsApp
  bridge is tied to a phone number, not a Funes user. The autoresponder stays
  admin-managed.

- ~~**Consolidation across users?**~~ **Decided: per-user**, as recommended.
  `consolidate()` with no user named iterates each pool in turn. Two people
  stating the same fact are two facts; merging them would write one person's
  wording into the other's memory and delete a row that was never theirs.

---

## Estimated scope

The core C++ codebase is ~6,000 lines. The changes are mechanical but
pervasive.

| Phase | Files touched | Nature |
|-------|--------------|--------|
| 1. Auth | `memory.h/cpp`, `api.h/cpp`, `main.cpp` | New code: ~400 lines (users table, auth middleware, login/logout endpoints) |
| 2. Data isolation | `memory.h/cpp`, `agent.h/cpp`, `api.cpp`, `tools.h`, all tool `.cpp` files | Thread user_id through ~30 function signatures, add WHERE clauses to ~20 queries |
| 3. Workspace | `file_tools.cpp`, `shell_tool.cpp`, `fs_guard.cpp`, `api.cpp` | Small: resolve workspace root per user instead of globally |
| 4. Permissions | `api.cpp`, `agent.cpp`, `tools.cpp` | Permission checks at API and tool-dispatch layers; ~200 lines |
| 5. UI | `ui/` directory | Login page, user indicator — frontend work |

---

## Migration safety

The existing `migrate()` function in `memory.cpp` already uses
`add_column_if_missing()` for incremental schema changes — same pattern here.
The migration is:

1. Create `users`, `auth_tokens`, and `jid_users` tables
2. Insert user 1 (admin) — username from `FUNES_ADMIN_USER` env var or prompt
   at first startup
3. Add `user_id` columns with `DEFAULT 1` to every existing table
4. Rebuild unique constraints (SQLite requires table recreation for constraint
   changes — wrap in a transaction)
5. Move workspace files to `data/workspaces/1/`

Existing single-user databases upgrade in place. The `DEFAULT 1` on every new
column means all existing data is attributed to the admin.
