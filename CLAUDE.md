# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Funes is a self-hosted AI assistant with persistent memory: a single C++17
binary (`funes`) that serves the web UI, chat API, agent runtime, and a
SQLite-backed memory engine. No Python/Node/containers/external DB at
runtime — Python does not appear at runtime at all; the
`funes-julio` extension brings its own where it needs it.

As of 4.0 it is multi-user (households and small teams): every `/api/` route
requires authentication, and memories, conversations, stored tool results,
cron jobs and workspace files all belong to one account.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Requires `libssl-dev`, `libyaml-cpp-dev`. Everything else (SQLite, sqlite-vec,
httplib, json, MCP client) is vendored under `third-party/`. The binary lands
in `bin/funes` (not `build/`) so it can find `./ui` and `./agents` relative to
the project root — always run it from the repo root.

## Run

```bash
./bin/funes                     # → http://localhost:8484
```

Needs an LLM endpoint reachable via `FUNES_LLM_URL` (OpenAI-compatible or
Anthropic). See `config/funes.conf` for every setting; put secrets in
`config/funes.local` (gitignored) — never in a committed file or an agent
YAML's `env:` block.

On a fresh database every endpoint answers 401 until an admin account exists.
Create one from the web UI's first-run screen, or from the CLI:

```bash
./bin/funes useradd julio --admin
```

Same binary, other subcommands: `userdel`, `userlist`, `passwd`, `perms`,
`locale`, `jid-map`, `jid-unmap`, and one maintenance command, `cron-cleanup`.
(`locale` is how an admin sets *another* account's language —
`PUT /api/me/locale` only ever changes the caller's own, so a new account
can't be set up in its owner's language before they first log in without it.) They run
against `FUNES_DB` and exit without starting the server or touching the LLM.
Passwords are always prompted, never taken as arguments.

## Tests

```bash
cd build && ctest --output-on-failure     # all C++ unit tests (+ an extension's, if built in)
ctest -R memory                            # single test, e.g. test_memory.cpp -> `memory`
bash tests/integration.sh                  # full-stack integration test, mock LLM, no network
```

Test names in `ctest -R <name>` are the `test_*.cpp` filename with the
`test_` prefix stripped (registered in `tests/CMakeLists.txt`). Each unit test
is a standalone binary with its own `main()` and a minimal `CHECK()` macro —
no external test framework. With `-DFUNES_EXTENSIONS=...` the extension's
tests join the same `ctest` run (prefixed `julio_`).

There is no separate lint step; treat `cmake --build build` warnings and
`ctest` as the correctness gate.

`tests/test_user_isolation.cpp` is the one to keep green above all others:
it asserts that no account can read, delete or overwrite another's memories,
turns, summaries, stored results, cron jobs or files. It also covers
*fidelity* — a user still gets all of their own recall results when another
account's pool is far larger — which is a different failure from a leak and
the reason `vec_memories` is partitioned.

## Architecture

### The agent loop is the core of the system

`src/core/agent.cpp` (`FunesAgent`, one instance per request) runs: recall
memories → load history/summary → compress context if needed → enter a
tool-calling loop against the LLM → persist turns. Every tool call passes
through several independently-developed safety layers, each catching a
specific production failure mode (see `src/core/README.md` for the full
list with incident context):

1. **Tool call recovery/withholding** (`llm_client.cpp`) — local models
   sometimes emit tool calls as prose/XML instead of native calls; recovered
   normally, discarded when the tool was deliberately withheld.
2. **Tool budget** (`tool_budget.cpp`) — per-tool call ceilings
   (`tool_limits` in agent YAML). Refusal is recoverable, not fatal.
3. **Loop detection** (`agent.cpp`) — exact-repeat and near-duplicate call
   detection kills runaway runs.
4. **Last-step reservation** — the final step always withholds tools and
   forces a written synthesis instead of one more tool call.
5. **Completion contract** (`completion_contract.cpp`) — `require_tools` in
   agent YAML: named tools must succeed before a text answer is accepted.
6. **Answer schema** (`answer_schema.cpp`) — `answer_schema` in agent YAML:
   validates the final answer's JSON shape before accepting it.
7. **Run outcome** (`run_outcome.cpp`) — every failure exit is a
   `FAILED — ...` string; `delegation.cpp` turns that into a tool *error*
   for the caller, not content it might repeat as fact.
8. **Result store** (`result_store.cpp`) — tool outputs over 2KB are stored
   out-of-transcript; the model sees a head/tail preview + `result_id` and
   dereferences on demand via `read_result`, so a big scrape can't eat the
   context window or become the answer verbatim.

When touching the agent loop, know which of these layers a change interacts
with — they compose, and several were added specifically because an earlier
layer alone wasn't sufficient (e.g. withholding a tool's schema, not just
telling the model not to use it, because local models reach for the wrong
thing anyway).

### Agents are config, not code

An "agent" (`agents/*.yaml`) is a name + system prompt + tool allowlist run
by the one shared runtime in `agent.cpp` — not a separate process. `funes` is
the only agent the user talks to; it delegates to specialists via
`delegate_to_agent(agent, task)` (self-delegation refused, depth-2 cap) and
relays the result in its own voice. The `task` string must be fully
self-contained — the specialist never sees the calling conversation.
Dropping a new YAML into `agents/` + `POST /api/agents/reload` makes it live
with no rebuild; a new *tool* (C++) does need a rebuild. See `agents/README.md`
for the full roster and the hub-and-spoke diagram.

### Tools

`src/core/tools/` — one file per tool (or tool pack), self-registering into
`ToolRegistry` (`tools.h/cpp`) at startup, no protocol/HTTP hop for built-ins.
External tools plug in via MCP (`mcp_servers:` in agent YAML, SSE or stdio —
stdio servers are spawned fresh as subprocesses per request). Shared
infrastructure tools build on: `fs_guard` (workspace path confinement),
`net_guard` (SSRF blocking), `process_runner` (fork/exec with timeout and
output cap). See `src/core/tools/README.md` for the full inventory and the
security model.

Two rules there that are easy to undo by accident:

- **Children get an allowlisted environment, never this process's.**
  `funes_config.h` loads `funes.local` into the environment, so "inherit
  everything" hands `FUNES_SERVICE_TOKEN` and every API key to any script,
  shell command or MCP stdio server a model starts. `proc::child_environment()`
  is the one place that decides what passes (PATH, HOME, locale, proxies,
  Python/CA plumbing, plus `FUNES_CHILD_ENV`); `run_argv`, `run_shell_command`
  and the vendored `mcp::stdio_client` (via `set_environment_filter`, wired in
  `main.cpp`) all go through it. A secret a program needs goes in the script
  manifest's `env:` or the MCP server's `env:` — one program, one secret.
- **`net_guard::is_private_host` resolves the name and judges the addresses.**
  A regex over the host literal stopped `127.0.0.1` and nothing else that
  meant it (`127.0.0.1.nip.io`, `0x7f000001`, `[::ffff:127.0.0.1]`, a domain
  pointed at the LAN). Unresolvable names are refused, not waved through.
  Redirects are followed by hand (`page_text.cpp`, `net::resolve_location`)
  so every hop is checked — httplib's own `set_follow_location` skips the
  guard, and is off everywhere.

### Extensions: one operator's tools live outside this repository

`src/core/extension.h`. A directory passed as `-DFUNES_EXTENSIONS=<dir>` (a
semicolon-separated list) provides `extension.cmake`, which adds its sources to
the `funes` executable; each source queues a registration function from a
static initializer and `main()` runs the queue after the built-in tools, handing
it `{tools, memory, workspace_dir}` and nothing else. `FUNES_AGENTS_DIR` is
colon-separated for the same reason, so an extension's `agents/` loads beside
the repo's seven (later directories shadow earlier ones by name; `create_agent`
writes into the first). Rule: the core never names an extension's tool, file or
directory — if a core header needs to know about one, that is a flag on the
registration (`NativeTool::inline_result` is the example), not a string.

The first extension is `funes-julio` (sibling repository): the newsletter
(`harvest_candidates`/`publish_issue`, `publications/`, `publishing/`), the
VoC pipeline (`write_structured`/`read_structured`/`merge_rankings`,
`pipelines/`), and the agents that use them plus WhatsApp, Gmail, RSS and the
book editor. Its README carries the "don't ask the model for what a tool can
supply" incident write-up that used to be here; the principle stays in the
core — `answer_schema`, `require_tools`, `tool_limits`, `scriptlib`'s `output:`
contract — the worked example moved with the code it was about.

### Scripts an agent may run

`scriptlib/` (`FUNES_SCRIPTS_DIR`) is the central script library, and
`src/core/script_library.{h,cpp}` + `src/core/tools/script_tools.cpp` are what
reach it. One manifest (`<name>.yaml`: description, `run:`, optional
`interpreter:`, typed `params:`, `env:`) plus one executable file per script.
An agent gets a script the way it gets a tool — by name, in its YAML:

```yaml
tools:   [..., list_scripts, run_script]
scripts: [workspace_report, backup_workspace]
```

Three things to keep straight when touching this:

- **`scripts:` denies by default.** Empty or absent means *no scripts*, the
  opposite of `tools:`. A file appearing in a directory must not become
  runnable by every agent; each grant is written down. `ToolContext`
  defaults the list to empty for the same reason — a call site that doesn't
  know about scripts grants none rather than all.
- **The model supplies arguments, never a command.** Declared parameters
  become `--name=value` argv tokens passed to `execvp`; there is no shell, so
  `;` in a value is punctuation. That is the whole reason an agent can be given
  one job to run without being given `execute_shell`, and `FUNES_ALLOW_SHELL`
  deliberately does not gate `run_script`.
- **The library sits outside every workspace.** `read_file`, `write_file` and
  `/api/upload` are confined to `<workspace>/<user_id>/` by `fs_guard`, so
  nothing the model drives can read, edit or add a script — installing one is
  an admin editing the repo, like adding an agent. A script still runs with the
  process's own permissions: this is an allowlist, not a sandbox.

Three things compose with the layers above rather than sitting beside them:

- **Qualified call keys** (`funes::call_keys`, `tool_budget.h`). `run_script`
  is one tool standing in for n programs, so a call also counts against
  `run_script:<script>`. `tool_limits`, `require_tools` and a user's
  `permissions` blob all accept either form — without it three granted scripts
  would share one budget and `require_tools: [run_script]` would be satisfied
  by whichever script happened to run. The aggregate key withdraws the tool
  from the schema when spent; a spent script is refused by name so the others
  stay callable.
- **A declared output shape** (`output:` in the manifest, `validate_output`).
  The pipeline-stage contract applied to a script: `format: json` plus the
  `answer_schema.h` subset, checked before the model sees the result, with the
  error worded as an installation fault so the model reports it instead of
  retrying. A JSON script's stderr is captured separately
  (`run_argv(..., separate_stderr)`) — otherwise one library warning fails a
  schema the script satisfied.
- **`schedule_job(kind="script")`** (`cron_runner.cpp`). Unattended work no
  longer has to be `kind: "shell"` and so no longer needs `FUNES_ALLOW_SHELL`.
  It dispatches through the same `run_script` tool an interactive call uses —
  one set of rules, not a second path nobody watches — and re-resolves both the
  agent's grant and the owner's permissions *at fire time*, so revoking either
  stops the timer. `kind="shell"` follows the same rule since 5.0.1: it is
  `execute_shell` deferred, so it needs the owner to be permitted
  `execute_shell` both when scheduled and when fired, on top of the global
  switch. Before that it checked only the switch — a member denied the shell
  could schedule one anyway (`tests/test_cron_tool.cpp`,
  `test_shell_jobs_need_execute_shell_permission`).

The agent loop lists an agent's granted scripts in its system prompt
(`AgentDefaults::scripts_dir`), filtered by the caller's permissions, the way
it lists the delegation roster — a capability a small model has to discover
with a tool call is one it will instead guess at, usually by reaching for the
shell. See `scriptlib/README.md`.

### Memory

`src/core/memory.cpp` (`MemoryStore`) — SQLite + vendored sqlite-vec, one
file (`~/.funes/memory.db` by default). Semantic search when an embedding
endpoint is configured, keyword fallback otherwise (and automatic vector
backfill once one appears). `Memory::source` (`user`/`tool`/`auto`/
`consolidated`) affects recall ranking, not just display —
`MemoryStore::source_weight` boosts deliberately-taught facts over passive
conversation log entries at equal cosine similarity. A background pass
consolidates near-duplicates and prunes stale never-recalled `auto` memories
(`FUNES_CONSOLIDATE*` env vars); explicit `user` memories are never pruned,
and merging only ever happens within one account's pool.

What a run *writes* is three-valued, not a bool: `FunesAgent::run` takes a
`Persist` of `Full` (a person talking), `TurnsOnly` (a scheduled job — keep the
transcript, write no auto-memory) or `None` (a delegated sub-agent call). The
middle one exists because a cron firing used to store `User said: "<the job's
task>" — I replied: "..."`, and those got recalled into real conversations as
things the person had said. Scheduled sessions (`cron-<id>-<epoch>`) are hidden
from `list_sessions` unless asked for; `funes cron-cleanup` removes what an
older database already holds.

As of 5.0 recall is no longer a single flat similarity search. Two passes run
after it and **only ever append** to what it found: a one-hop expansion through
`memory_links` (an explicit "these belong together", stored between two of one
account's memories), and an FTS5 term match (`memories_fts`, an external-content
index kept in sync by triggers). Ordering it that way is what makes "connected
memories cannot make recall worse" a property of the code rather than a hope
about weights — an expanded hit inherits its anchor's score times the link
weight times a decay below 1, so it can never outrank the memory it was reached
through, and term matches enter below the weakest direct hit. Links come from
`backfill_links()`, a capped, resumable background pass that asks the model
about one candidate pair at a time (`FUNES_LINK_BACKFILL=on`; off by default
because it is one model call per pair on a GPU the chat path needs). Every
verdict is recorded in `memory_link_judgments`, including the negatives — a
capped run that re-asked the questions it already answered would never reach
the end of the pool.

Each agent has an isolated memory namespace by name unless it sets
`memory_scope:` to share another agent's pool (the funes-julio
WhatsApp autoresponder uses it to share `funes`'s memory). That is orthogonal to
the per-user scoping below: `memory_scope` picks *which agent's* pool,
`user_id` picks *whose*.

### Users, authentication and isolation

`src/core/users.cpp` (`UserStore`) owns `users`, `auth_tokens` and
`jid_users` — its own class and connection on the same SQLite file, since
authentication has nothing to do with recall and `memory.cpp` is already
large. Passwords are PBKDF2-HMAC-SHA256 via OpenSSL (`src/core/password.cpp`),
which httplib already links for HTTPS, so auth added no dependency.

Two things to keep in mind when touching this area:

- **Isolation is enforced in the SQL, not at the API layer.** Every
  `MemoryStore` method takes the `user_id` it acts as, with no default value,
  and every query carries `WHERE user_id = ?`. The missing default is
  deliberate: a new call site that forgets to pass one fails to compile
  rather than silently writing into the admin's data. Ownership checks live
  on the statement itself (`DELETE ... WHERE id = ? AND user_id = ?`), so
  "not yours" and "not there" are the same answer and ids can't be probed.
- **Authentication is enforced twice.** A pre-routing gate in `api.cpp`
  refuses any unauthenticated `/api/` path so a route added later is
  protected by default; handlers separately resolve the caller via
  `require_auth` to scope what they return. The gate is the boundary, the
  per-handler lookup is what makes the answer correct.

`vec_memories` carries `user_id` as a vec0 **PARTITION KEY**, not an ordinary
column — vec0 refuses to combine `MATCH` with an arbitrary `WHERE`, but a
partition key it accepts, and prunes partitions rather than filtering after
the fact. Post-filtering a shared KNN result is not a leak but it degrades
silently: the candidate pool is drawn from everybody, so a busy account
crowds a quiet one out of its own top-k. Two related traps, both already hit:
the vec table is rebuilt during `migrate()` rather than lazily, because
`recall_semantic` names `v.user_id` and a recall usually happens before the
first write; and vec0 with a partition key rejects `INSERT OR REPLACE` on an
existing row, so `insert_vector` deletes then inserts.

Session tokens are stored hashed (`sha256_hex`, `password.h`): the cookie
value is the credential, the row is its digest, and a copy of `memory.db` is
not a set of live sessions. `/api/login` throttles per address and per
username after five failures (429 + `Retry-After`, doubling to five minutes)
before the PBKDF2 work is done, and the server caps request bodies at 64 MB
because httplib reads the body before the auth gate runs.

Identity reaches non-browser callers by service token: the WhatsApp
autoresponder sends `FUNES_SERVICE_TOKEN` plus the sender's jid, and Funes
maps the jid to a user. The token says the caller is trusted; the jid says
who for. Neither alone authenticates anything.

### Permissions

`src/core/permissions.cpp` — two roles plus an optional per-user allowlist in
`users.permissions`. `agents` absent/empty means all; a `tools` entry is
final, and a tool with no entry falls back to whether it is privileged
(`execute_shell`, `create_agent`, `create_tool` denied by default). Enforced
in four places, and all four are load-bearing: `/api/agents` filters,
`/api/chat` 403s, `FunesAgent::run` narrows the tool schema, and
`dispatch_tool` re-checks — because a model that can't *see* a tool will
sometimes write the call as prose, which `llm_client` rescues into a real
call. `delegate_to_agent` checks the agent allowlist too, or the restriction
is theatre. The roster an orchestrator sees is filtered by the caller's
permissions and split in two (`src/core/agent_roster.h`): available agents, and
denied ones named with the reason. An unfiltered roster was worse than none —
the model read about an agent it could not use, delegated, was refused, and
then improvised an explanation, usually blaming a server setting. The two
reasons need opposite advice: an allowlist denial an admin can grant, while an
agent declaring `shared_identity` (one mailbox, one phone number, one
subscriber list) is not something an admin *can* grant per account. Permissions only restrict: `filter_tools` intersects with the
agent's own list and expands an empty agent list first.

### Workspaces

`FUNES_WORKSPACE_DIR/<user_id>/` per account. `fs_guard::workspace_for` is
the single resolver for `read_file`, `write_file`, `execute_shell`'s cwd and
`/api/upload` — keep it that way, because if two of them disagree about the
root then `fs_guard::resolve`'s confinement check is guarding a different
directory than the one being written to. An agent's `workspace_dir` in YAML
nests inside the caller's workspace when relative; an absolute path is
honoured verbatim as a deliberate shared folder (nothing shipped uses it).

### Server

`src/server/` (`main.cpp`, `api.cpp`, `user_cli.cpp`, `maint_cli.cpp`) — one
binary, all routes plain HTTP/SSE. `/api/chat` streams `memories`, `delta`, `tool_call`,
`tool_result`, `result_stored`, `contract_nudge`, `schema_nudge`,
`context_compressed`, `usage`, `done`, `error` events over SSE. Four routes
are public (`/api/login`, `/api/logout` aside, `/api/auth/status`,
`/api/auth/bootstrap`); everything else needs a session cookie or a service
token. `bootstrap` is public but self-closing — it only works while no user
exists, which is how a fresh install gets its first admin without shipping a
default password. `PUT /api/me/locale` sets the caller's own language and nobody else's — a
route taking a user id would be account administration wearing a preference's
clothes, and that lives in the CLI. The agent runtime appends the
reply-language instruction itself (`AgentDefaults::user_locale`, resolved per
run from `UserStore`), rather than each agent YAML carrying
it: a locale change then takes effect everywhere at once and no prompt can be
left behind. English appends nothing — every prompt in the repo is already
English, so saying it again spends context to change nothing.

`/api/auth/status` also returns the caller's *resolved*
permissions (allowed agents, denied tools) so a member can see why an agent is
missing without an admin SSHing in — read-only, editing stays in the CLI. See
`src/server/README.md` for the full route table and startup sequence
(background threads: embedding backfill, result pruning, memory consolidation,
cron runner). The backfill thread *drains* rather than making one capped call:
one call is not the backfill, and before 4.0 a database with more than 256
memories missing vectors came back on keyword-only recall until the next
restart.

### UI

`ui/` — vanilla JS, no build step, no framework. Talks to the API above.
Localized as of 5.0: `ui/i18n/<locale>.json` string tables, a `t(key)` lookup
that falls back key → English → the key itself (a missing translation shows
English, a missing key shows the key, neither empties the button it was meant
to label), and `data-i18n` / `data-i18n-title` / `data-i18n-placeholder`
attributes translated in one idempotent pass so switching language needs no
reload. The account's stored `locale` wins over `navigator.language` — the
reverse would silently change a deliberate choice every time the person opened
the app on a different machine. The
auth gate covers the app until `/api/auth/status` resolves, switching between
a first-run "create the admin account" form and a plain sign-in. Session
expiry is caught by a single `window.fetch` wrapper rather than a check at
each call site — there are a dozen callers, several on timers, and the
failure surfaces at whichever fires first.

## Directory map

```
agents/        the seven shipped agent YAMLs (see agents/README.md)
config/        funes.conf (committed defaults) + funes.local (gitignored secrets)
scriptlib/     central script library: what agents may run by name (see scriptlib/README.md)
src/core/      the agent harness (LLM loop, memory, users/auth, tools, safety, extension hook)
src/core/tools/  individual tool implementations
src/server/    HTTP API + SSE + entry point + the admin user CLI
tests/         C++ unit tests + bash integration test + mock LLM
third-party/   vendored: sqlite, sqlite-vec, cpp-mcp (httplib, json)
ui/            web UI (vanilla JS)

Not here any more (5.1): pipelines/, publications/, publishing/, scripts/, the
WhatsApp and IMAP MCP servers, and eleven agents — all in the funes-julio
extension repository.
```

Nearly every directory has its own `README.md` with more detail than this
file carries — read the local one before making non-trivial changes in that
area (`src/core/README.md`, `src/core/tools/README.md`, `src/server/README.md`,
`agents/README.md`, `config/README.md`, `scriptlib/README.md`,
`tests/README.md`).

`docs/ROADMAP.md` says what is being built next and in what order.
