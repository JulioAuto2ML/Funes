# Funes Technical Reference

Architecture, internals, and design decisions for the Funes agent harness.

---

## Table of contents

1. [Architecture overview](#architecture-overview)
2. [The harness concept](#the-harness-concept)
3. [Request lifecycle](#request-lifecycle)
3b. [Users, authentication and isolation](#users-authentication-and-isolation)
4. [The agent loop](#the-agent-loop)
5. [Safety mechanisms](#safety-mechanisms)
6. [Memory system](#memory-system)
7. [Tool system](#tool-system)
8. [LLM client](#llm-client)
9. [Agent delegation](#agent-delegation)
10. [The newsletter pipeline](#the-newsletter-pipeline)
11. [Context management](#context-management)
12. [Cron scheduler](#cron-scheduler)
13. [MCP integration](#mcp-integration)
14. [Build system](#build-system)
15. [Testing architecture](#testing-architecture)
16. [Design principles](#design-principles)

---

## Architecture overview

Funes is a monolithic C++ binary (~10,000 lines of core + server code) that
serves the web UI, the REST/SSE API, the agent runtime, and the memory engine.

```
                   ┌─────────────────────────────┐
                   │        Web Browser           │
                   │   (ui/index.html + app.js)   │
                   └──────────┬──────────────────┘
                              │ HTTP / SSE
                   ┌──────────▼──────────────────┐
                   │     src/server/ (httplib)     │
                   │  FunesApi: routes + agents    │
                   └──────────┬──────────────────┘
                              │
          ┌───────────────────▼───────────────────┐
          │           src/core/agent.cpp           │
          │    FunesAgent: the harness loop         │
          │                                         │
          │  ┌─────────┐ ┌──────────┐ ┌─────────┐ │
          │  │LLMClient│ │MemoryStore│ │ToolReg  │ │
          │  └─────┬───┘ └────┬─────┘ └────┬────┘ │
          └────────┼──────────┼────────────┼──────┘
                   │          │            │
          ┌────────▼───┐ ┌───▼────┐  ┌────▼──────────────┐
          │ LLM Backend│ │ SQLite │  │ Tools (in-process) │
          │(llama.cpp, │ │+ vec   │  │ + MCP servers      │
          │ OpenAI,    │ └────────┘  │   (external procs) │
          │ Anthropic) │             └────────────────────┘
          └────────────┘
```

### Key dependencies

| Dependency | Bundled | Purpose |
|---|---|---|
| SQLite + sqlite-vec | Yes (third-party/) | All persistent state |
| cpp-mcp (httplib + nlohmann/json) | Yes (third-party/) | HTTP server/client, JSON, MCP protocol |
| OpenSSL | System | HTTPS for web_fetch |
| yaml-cpp | System | Agent config parsing |

---

## The harness concept

A **harness** is the deterministic program that wraps an LLM and controls
everything the model cannot control about itself:

- **When it runs**: the harness schedules agent executions (cron, HTTP request)
- **What tools it can use**: the harness filters tool schemas by allowlist
- **When it must stop**: the harness enforces step budgets, loop detection, and
  tool ceilings
- **What counts as success**: the harness checks completion contracts and answer
  schemas
- **What happens on failure**: the harness produces structured failure signals
  detectable by delegation

The model produces text and tool calls. The harness decides whether to execute
them, what the model sees next, and whether the run is done.

This design exists because LLMs are unreliable in specific, observable ways:
they stop early, loop on the same tool, ignore refusals, and produce
plausible-sounding answers without doing the work. Each safety mechanism in
Funes was developed in response to a specific production failure, documented
in comments with dates and root causes.

---

## Request lifecycle

1. HTTP POST to `/api/chat` with agent name, session ID, and user message
2. The pre-routing auth gate resolves a session cookie or service token, or
   answers `401` before any handler runs
3. The handler resolves the caller again (`require_auth`) to get the `user_id`
   it will scope everything to
4. `FunesApi` looks up the `AgentConfig` from the agent table
5. Creates a `FunesAgent` with the config, shared `ToolRegistry`, `MemoryStore`,
   and `AgentDefaults`
6. Calls `FunesAgent::run(message, session, user_id, ...)` inside a chunked SSE
   response, which builds a `ToolContext` carrying `user_id` into every tool
   call — including `delegate_to_agent`, which passes it to the sub-agent
5. `run()` executes six phases:
   - **Recall**: semantic search for relevant long-term memories
   - **Load history**: rolling summary + recent turns from the session
   - **Compress**: if estimated tokens > 70% of context_limit, fold oldest
     half of turns into the summary
   - **Build prompt**: system prompt + agent roster + summary + memories +
     history + user message
   - **Tool loop**: LLM completion -> tool dispatch -> repeat (with all safety
     checks)
   - **Persist**: store turns in session history, auto-memorize the exchange
7. Final answer streamed via SSE `done` event

---

## Users, authentication and isolation

Added in 4.0. Two claims are worth stating precisely, because the
implementation depends on both.

**Authentication is enforced twice, deliberately.** `FunesApi::mount` installs
a pre-routing handler that refuses any `/api/` path not on a small public
allowlist (`/api/login`, `/api/auth/status`, `/api/auth/bootstrap`). Each
handler *also* calls `require_auth`. The gate is the security boundary — it
means a route added later is protected without anyone remembering to protect
it — and the per-handler call is what supplies the identity the response is
scoped to. Neither replaces the other.

**Isolation is enforced in SQL, not in handlers.** Every `MemoryStore` method
takes the `user_id` it acts as, with no default value, and every statement
carries `WHERE user_id = ?`. The absent default is the point: a call site that
forgets to pass one is a compile error rather than a silent write into the
admin's data. Ownership checks live on the mutating statement itself —
`DELETE FROM memories WHERE id = ? AND user_id = ?` — which avoids a
check-then-act race and makes "not yours" and "not there" the same observable
result, so ids can't be enumerated.

### Storage

| Table | Owner column | Notes |
|---|---|---|
| `users` | — | id, username (unique, case-insensitive), display_name, password_hash, role, permissions JSON |
| `auth_tokens` | `user_id` | Opaque 256-bit hex session tokens with an expiry. `ON DELETE CASCADE` |
| `jid_users` | `user_id` | WhatsApp `chat_jid` → account. `ON DELETE CASCADE` |
| `memories` | `user_id` | `UNIQUE(user_id, agent, text)` — the pre-4.0 `UNIQUE(agent, text)` made one user's fact block everyone else's |
| `turns`, `session_summaries`, `tool_results`, `cron_jobs` | `user_id` | Session ids are client-supplied strings, so two accounts colliding on one is expected and must stay separate |
| `vec_memories` | `user_id` **partition key** | See below |

`UserStore` owns the first three on its own connection to the same SQLite
file. Authentication has nothing to do with recall, and `memory.cpp` was
already large.

### The vector index

`vec_memories` declares `user_id` as a vec0 **PARTITION KEY** rather than an
ordinary column. vec0 refuses to combine `MATCH` with an arbitrary `WHERE`
clause — which is why `recall_semantic` originally ran KNN across every row
and filtered afterwards in C++, over-fetching `k*8` to survive the filter. A
partition key is the exception vec0 does accept, and it prunes partitions
instead of post-filtering.

This matters even though post-filtering leaks nothing: the candidate pool is
drawn from every account, so a heavy user crowds a light one out of their own
top-k. The symptom is recall quietly getting worse as accounts are added, and
it would be blamed on the model. `test_user_isolation` asserts against it
directly with a 200:1 pool imbalance.

Two consequences that cost real debugging:

- **The rebuild happens in `migrate()`, not lazily.** `ensure_vec_table` is
  otherwise only reachable through `insert_vector`, but `recall_semantic`
  names `v.user_id` — and a recall normally precedes the first write, since
  recall runs at the top of every turn. Against a pre-4.0 table that is a
  failed `sqlite3_prepare_v2`, which throws and takes the agent run with it.
- **`insert_vector` deletes then inserts.** vec0 with a partition key rejects
  `INSERT OR REPLACE` on an existing row (`UNIQUE constraint failed on v
  primary key`). The only caller that hits an existing row is consolidation
  re-vectorising a merged memory, so the symptom was a logged warning and a
  merged memory silently dropping to keyword-only recall.

### Password hashing

PBKDF2-HMAC-SHA256 via OpenSSL (`src/core/password.cpp`), 600k iterations,
16-byte random salt. OpenSSL is already a hard dependency — httplib links it
for HTTPS — so authentication added none. The stored form is self-describing,
`pbkdf2_sha256$<iterations>$<b64 salt>$<b64 hash>`, and verification reads the
iteration count out of the string rather than assuming the current default, so
the cost can be raised without invalidating existing hashes. Every parse
failure returns false rather than throwing: an empty or damaged
`password_hash` must not mean "any password works".

### Identity for non-browser callers

The WhatsApp autoresponder sends `X-Funes-Service-Token` plus
`X-Funes-User-Jid`. The token establishes that the *caller* is trusted; the
jid says *who the request is for*, and is resolved through `jid_users`.
Neither authenticates anything alone — a valid token with no jid, or with an
unmapped one, is a `401`. That is what stops a message from an unknown number
being answered as the admin.

### Background work

Work with no request to take an identity from carries its own. Cron jobs
record their owner at creation and `cron_runner` adopts it for the run;
consolidation iterates one account's pool at a time, because two people
stating the same fact are two facts and merging them would write one person's
wording into the other's memory.

### Workspaces

`FUNES_WORKSPACE_DIR/<user_id>/`, resolved by `fs_guard::workspace_for` —
the single resolver shared by `read_file`, `write_file`, `execute_shell`'s cwd
and `/api/upload`. Keeping it single is a correctness requirement, not tidiness:
if two of those disagree about the root, `fs_guard::resolve`'s confinement
check is guarding a different directory than the one being written to. An
agent's `workspace_dir` nests inside the caller's workspace when relative; an
absolute path is honoured verbatim as a deliberate shared folder, the
filesystem counterpart of `memory_scope`.

---

## The agent loop

The core loop in `FunesAgent::run_loop()` (agent.cpp) alternates between LLM
completions and tool dispatch. At each step, the harness checks:

### Step-by-step flow

```
for step in 0..max_steps:
    if step == max_steps - 1:
        withhold all tools, send "synthesize now" notice

    completion = llm.chat(history, tools_schema)

    if completion has tool calls:
        for each tool call:
            if tool over budget (tool_limits):
                refuse call, withhold tools on next step
            elif tool call is exact repeat (3x):
                kill run -> loop_failure
            elif same tool called too many times:
                kill run -> loop_failure
            else:
                result = dispatch_tool(name, args)
                if result exceeds 2KB:
                    store in result_store, inline preview only
                track success/failure for completion contract

    elif completion has text answer:
        if completion contract not satisfied:
            inject nudge, continue loop
        if answer_schema defined and answer doesn't match:
            inject schema nudge, continue loop
        return answer  # success

    elif completion is empty:
        retry once with tools withheld
        if still empty: return empty_answer_failure

return max_steps_failure
```

### Vision routing

When images are attached and `FUNES_VISION_URL` is configured, the first
completion goes to the vision model. Subsequent steps (tool calls, reasoning)
continue on the main model, which is typically better at multi-step work.

### Tool call recovery

Local models sometimes write tool calls as JSON or XML in the content field
instead of using the API's native tool_calls mechanism. The LLM client
recovers these by parsing JSON objects with `name`/`arguments` fields or
`<tool_call>` XML blocks.

Critically, recovered calls are **discarded** when tools are withheld. Without
this, a model could bypass budget refusals or last-step reservations by
writing tool calls in prose.

---

## Safety mechanisms

### 1. Completion contract (`completion_contract.h`)

Declares tools that must have succeeded before a text answer is accepted.

```yaml
# In agent YAML:
require_tools: [harvest_candidates, publish_issue]
```

The `satisfied` set uses insert/erase semantics: a later failing call to the
same tool undoes an earlier success. This prevents the bug where
`ai-newsletter` could succeed on a first delegation, fail on the second, but
the contract was already satisfied by the first.

When the model tries to answer with outstanding requirements, a nudge is
injected: "Stop. That was a text answer, but X has not succeeded yet..."

If the nudge budget (required.size() + 2) is exhausted, the run produces
`FAILED -- ...` with the model's unverified claim included for diagnosis.

### 2. Tool budget (`tool_budget.h`)

Per-tool call ceilings that refuse over-budget calls with a recoverable error.

```yaml
tool_limits:
  web_search: 3
  web_fetch: 4
```

After a refusal, the harness **withholds tool schemas entirely** on the next
completion (`tool_choice: "none"` + schema dropped + explicit notice). This
was added because the 9B model re-issued a refused web_search seven times in
a row, each refusal costing a step, until max_steps ended the run with no
answer (2026-07-31).

### 3. Loop detection (agent.cpp)

Two detectors:

- **Exact-signature**: same (tool_name, args) pair called 3 times -> kill
- **Near-duplicate**: same tool_name called more than max(max_steps, 6) times
  -> kill

### 4. Last-step reservation (agent.cpp)

The final step always withholds tools and sends a "give a final answer now"
notice. This ensures the model synthesizes from what it has rather than
making one last tool call and running out of budget.

### 5. Run outcome (`run_outcome.h`)

Every failure exit produces a message starting with `FAILED -- ` (the
`kFailureMarker`). `delegation.cpp` checks for this prefix and turns it into
a tool error rather than passing it up as content.

This prevents the old bug where "a raw web-search dump once travelled three
levels up a delegation chain and was served to the user as a newsletter."

### 6. Answer schema (`answer_schema.h`)

JSON shape validation on the final answer. Supports: `type`, `required`,
`properties`, `items`, `enum`, `minItems`/`maxItems`. The validator returns
one concrete violation per nudge (not a list, which makes small models
"rewrite everything at once and reliably break something that was already
correct").

Schema nudges and contract nudges share one budget.

### 7. Result store (`result_store.h`)

Tool outputs exceeding 2 KB are stored out-of-transcript with a head/tail
preview and a `result_id`. The `read_result` tool retrieves windowed portions.

Two tools are exempt from storage: `read_result` itself (to avoid ID chains)
and `harvest_candidates` (its output is a menu meant to be read whole).

### Layering summary

```
Innermost                                             Outermost
    │                                                     │
    ▼                                                     ▼
 tool-call    tool      loop       last-step  completion  answer   run      result   context
 recovery +   budget    detection  reserve    contract    schema   outcome  store    compression
 withholding
```

Each layer catches a specific failure mode that the others miss. The layers
were developed incrementally from production incidents, not designed upfront.

---

## Memory system

### Storage (memory.h)

All persistent state lives in one SQLite database. Every table below carries
a `user_id` (see [Users, authentication and
isolation](#users-authentication-and-isolation)); `users`, `auth_tokens` and
`jid_users` are owned by `UserStore` on its own connection to the same file.

| Table | Purpose |
|---|---|
| `memories` | Long-term facts with source, timestamps, recall bookkeeping. `UNIQUE(user_id, agent, text)` |
| `vec_memories` | sqlite-vec virtual table for KNN semantic search, partitioned by `user_id` |
| `turns` | Conversation history per session |
| `session_summaries` | Rolling summary per session |
| `tool_results` | Large tool outputs stored by reference |
| `cron_jobs` | Scheduled recurring jobs, each with an owner the runner adopts |
| `users` / `auth_tokens` / `jid_users` | Accounts, session tokens, WhatsApp identity |

### Semantic search

When an embedding endpoint is available, `recall()`:
1. Embeds the query
2. Runs KNN **inside the caller's partition**, over-fetching 8x candidates
3. Applies source-dependent weights: `user`/`tool` get 1.3x, `consolidated`
   get 1.15x, `auto` get 1.0x
4. Returns the top k results

Step 2's partition scoping is what keeps the over-fetch a per-user budget
rather than one shared across every account — the agent filter is still
applied afterwards, since `agent` is not a partition key, which is why the
over-fetch remains.

This ensures deliberately taught facts outrank auto-captured conversation logs
at the same raw similarity.

### Graceful degradation

If the embedding endpoint goes down:
- `embedder_ok_` is set to `false`
- `recall()` falls back to `LIKE`-based keyword search
- `backfill_embeddings()` fills in missing vectors when the endpoint recovers
- If the embedding model changes dimension, the vec table is rebuilt

### Consolidation

Every 6 hours (configurable), a background thread:
1. **Merges near-duplicates**: clusters by cosine >= 0.92, sends each cluster
   to an LLM for a one-line merge. Each cluster is its own SQLite transaction.
2. **Prunes stale auto-memories**: deletes `source='auto'` memories with
   `recall_count=0` older than 30 days. `user` and `consolidated` memories
   are protected.

### Memory scoping

By default, each agent has its own isolated memory pool (scoped by agent name).
An agent can share another's pool via `memory_scope`:

```yaml
memory_scope: funes  # share the main assistant's memory
```

This is how `whatsapp-autoresponder` knows what the user has told funes.

---

## Tool system

### Registry (tools.h)

Tools are plain C++ functions registered at startup:

```cpp
struct NativeTool {
    std::string name;
    std::string description;
    json        parameters;   // JSON Schema for arguments
    ToolHandler handler;      // function(json args, ToolContext ctx) -> ToolResult
};
```

The registry generates OpenAI-format tool definitions, filtered by the agent's
allowlist. Handler exceptions are caught and turned into error ToolResults --
a tool failure never crashes an agent run.

### Tool context

Every tool call receives a `ToolContext` with:
- `agent`: which agent is calling
- `session`: which chat session
- `workspace_dir`: per-agent workspace override
- `memory_scope`: which memory pool to use

### Security layers

| Layer | Mechanism | Protects against |
|---|---|---|
| Filesystem | `fs_guard` (path confinement) | Path traversal, symlink escapes |
| Network | `net_guard` (SSRF protection) | Requests to localhost/private IPs |
| Shell | Opt-in flag + timeout + process-group kill | Unauthorized execution, runaway processes |
| Content | Binary rejection + output caps + UTF-8 validation | Crashes from invalid data |

---

## LLM client

### Dual provider support (llm_client.h)

The `LLMClient` speaks to both OpenAI-compatible APIs (llama.cpp, Groq, OpenAI)
and Anthropic's native API. Each provider has its own:
- Message serialization (role mapping, image encoding)
- Tool schema format (OpenAI uses `function`, Anthropic uses `input_schema`)
- Streaming SSE parsing

### Model-specific handling

- **Qwen**: tool results are reformatted as `<tool_response>` XML in user
  messages, because Qwen's chat template has no "tool" role.
- **Local models**: tool calls written as JSON/XML in content are recovered.
- **XML parsing**: manual O(n) scanning, not regex -- `std::regex` with
  `[\s\S]*?` blew the stack on large bodies.

### Tool withholding

When `tool_choice == "none"`, the tools array is **not sent at all** -- not
just flagged. This prevents the model from seeing tools it cannot use.

### Retry policy

Connection-level failures (not HTTP errors) are retried with backoff, up to
2 retries. Streaming retries only happen before any bytes have been received.

---

## Agent delegation

### How it works (delegation.cpp)

`delegate_to_agent(agent, task)` creates a new `FunesAgent` with the target's
config and runs it in-process:

- The sub-agent runs with `persist=false` -- its turns don't appear in the
  session history
- No streaming events -- the delegation is invisible to the UI
- The sub-agent's answer is returned as the tool result

### Safety

- Self-delegation refused
- `thread_local` depth guard caps delegation at 2 levels
- Unknown agents rejected with a list of available agents
- Sub-agent failures detected via `is_run_failure()` and marked as tool errors
  -- raw content never climbs the chain

### The roster

Agents with `delegate_to_agent` in their tool list get a dynamically generated
roster in their system prompt listing all other loaded agents (name +
description). This is wired at startup via a callback from `FunesApi`, so the
roster always matches `agents/*.yaml` as currently loaded.

---

## The newsletter pipeline

### Design: deterministic over agentic

The pipeline's motto: "only delegate steps that truly need model judgment,
everything else is code."

```
harvest_candidates (C++)          curator agent (LLM)        publish_issue (C++)
  Search (Tavily API)               Pick stories              Resolve IDs to URLs
  Deduplicate (URL + title)         Write post text            Grounding check (word overlap)
  Filter stale items                                           Link verification (HEAD/GET)
  Cap per-source, per-story                                    Render HTML + text
  Fetch every page                                             Send email (SMTP)
  Build numbered pool                                          Write run record
```

The model only does two things: pick which stories are interesting and write
the post text. Everything else is deterministic C++ or Python.

### The grounding check

`publish_issue` verifies each post's text against the candidate's page content
using sentence-level content-word overlap:

1. Split the page into sentences
2. For each post, find the sentence with the highest content-word overlap
3. If overlap < 3 words, reject the item
4. Before rejecting, try to substitute a different unused pool candidate whose
   page does support the text

This is purely deterministic -- no LLM involvement. It prevents the model from
hallucinating claims not supported by the source.

### Link repair

`publish_issue.py` checks every link. Broken links are automatically swapped
for same-story alternatives from the pool using content-word overlap matching.

---

## Context management

### Automatic compression (context_compressor.h)

When the estimated prompt exceeds 70% of `context_limit`, the oldest half of
loaded turns are sent to the LLM with a "summarize" system prompt. The result
replaces the rolling summary (it doesn't append -- the summary stays roughly
constant size).

### Token estimation

Uses `chars/4` as a model-agnostic proxy, plus 800 tokens per image.
Deliberately rough -- "good enough to decide 'are we getting close', not meant
to match any real tokenizer."

### Manual compression

The `compress_context` tool lets the model trigger compression on demand when
it notices the context getting full.

---

## Cron scheduler

### Architecture (cron_runner.h)

A background poll thread checks `MemoryStore::due_cron_jobs` every N seconds
(default 30). Two job kinds:

- **Agent jobs**: run `FunesAgent::run()` in-process with a unique session per
  execution. A notice is prepended warning there is no interactive user.
- **Shell jobs**: run a command via `process_runner` with timeout. Gated by
  `FUNES_ALLOW_SHELL`.

### Cron expressions (cron_schedule.h)

Standard 5-field format: minute, hour, day-of-month, month, day-of-week.
Supports: `*`, single values, comma lists, ranges, step values.

Day-of-month and day-of-week are OR'd when both are restricted (standard cron
semantics).

### Failure handling

Failed runs are recorded, not retried. A human or the `operator` agent notices
via `list_jobs` and can re-run with `run_job_now`. This follows the "check the
record, don't trust the model" pattern.

---

## MCP integration

Funes supports external [MCP](https://modelcontextprotocol.io) servers for
tools beyond the built-in set. Two transports:

### SSE (HTTP)

```yaml
mcp_servers:
  - name: my-server
    url: http://localhost:9000
```

### Stdio (child process)

```yaml
mcp_servers:
  - name: whatsapp-mcp
    command: /path/to/uv run main.py
    env:
      SOME_VAR: value
```

MCP tools are merged with the native registry, filtered by the agent's tool
allowlist. Native tools and earlier servers win on name collisions.

### Current MCP integrations

| Server | Transport | Used by |
|---|---|---|
| whatsapp-mcp | stdio (Python/Go) | whatsapp-assistant |
| imap-email-mcp | stdio (Node.js) | gmail-assistant |
| rss-reader-mcp | stdio (npx) | rss-reader, astro-ph-summarizer |

---

## Build system

### CMakeLists.txt

```
funes_core (static library)
  <- src/core/*.cpp
  <- third-party/sqlite (SQLite + sqlite-vec, compiled with SQLITE_CORE)

funes (executable)
  <- src/server/main.cpp + api.cpp
  <- funes_core
  <- src/core/tools/generated/*.cpp (GLOB CONFIGURE_DEPENDS)
  <- OpenSSL, yaml-cpp

tests (one binary per test file)
  <- funes_core
```

Generated tools (from `create_tool`) are picked up automatically by the GLOB
pattern, but require a rebuild to take effect.

---

## Testing architecture

### Unit tests

22 C++ test files, each a standalone binary with `main()`. No external test
framework -- a minimal `CHECK(cond)` macro. Key testing patterns:

- **FakeEmbedder**: 26-dimensional letter-frequency vectors for deterministic
  semantic similarity without a real embedding model
- **In-memory MemoryStore**: each test creates its own SQLite database
- **Direct function calls**: no HTTP, no server -- tests call functions directly

### Integration tests

`integration.sh` starts the real binary against `mock_llm.py` on scratch
ports. The mock uses keyword matching to trigger specific behaviors (loop,
budget burn, delegation failure, etc.), making tests deterministic without
a real LLM.

### Publishing tests

`publish_newsletter.py --self-test` runs the publishing test suite. Available
both via `ctest` and directly on the deployment machine.

---

## Design principles

These principles emerged from building and operating Funes, documented in
code comments with dates and incidents:

### 1. Deterministic over agentic

Only delegate to the LLM steps that truly need model judgment. Everything
else -- search, dedup, filtering, URL resolution, link checking, rendering,
sending -- is deterministic code that does not fail unpredictably.

### 2. Don't ask the model for what a tool can supply

URLs come from `harvest_candidates`, not from the model. The model picks by
numeric ID; the system resolves to URLs. This closes the path through which
hallucinated URLs could enter a published newsletter.

### 3. Verify, don't trust

The `run_publication.sh` script checks the run record file after the agent
reports success. `publish_issue` runs deterministic grounding checks on every
post. The harness checks completion contracts before accepting an answer.

### 4. Make failure explicit

Every failure exit produces `FAILED -- ...`, detectable by delegation. A run
that has no answer says so in a shape a caller can detect, rather than
surfacing the last tool result as if it were content.

### 5. Refuse recoverably, kill as backstop

Tool budget refusals are recoverable -- the model is told to conclude. The
loop detector kills the run outright. Having both means the model usually
produces something, and the backstop catches the cases where it doesn't.

### 6. Each safety mechanism is a response to a specific failure

Completion contracts exist because the model narrated a tool result as the
deliverable. Tool budgets exist because a research agent searched 20 times
without synthesizing. Tool withholding exists because the model re-issued
refused calls. Every mechanism has a dated incident in the comments.

### 7. One binary, one file, one config

The `funes` binary serves everything. Memory is one SQLite file. Configuration
is layered `KEY=value` files. No containers, no external databases, no package
managers at runtime.
