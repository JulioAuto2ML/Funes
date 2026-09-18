# Funes

**A personal assistant that learns and remembers.**

> *“I have more memories than all mankind since the world began.”*
> — Jorge Luis Borges, *Funes the Memorious*

Funes is a self-hosted AI assistant with one defining trait: **persistent memory**.
Tell it something today and it will know it tomorrow, next week, in a different
conversation. Memory is not a bolt-on — it is the product. Every answer starts by
recalling what Funes knows about you, and the web UI shows you exactly what it
remembers (and lets you delete any of it).

- **Fast C++17 backend, one binary.** The `funes` binary serves the web UI, the
  chat API, the agent runtime, and the memory engine. No Python, no Node, no
  containers, no external database.
- **Memory in a single file.** SQLite + [sqlite-vec](https://github.com/asg017/sqlite-vec)
  (vendored). Semantic search when an embedding model is available, keyword
  search when it isn't. Your entire memory is `~/.funes/memory.db` — back it up
  with `cp`.
- **Works with any LLM.** OpenAI-compatible endpoints (llama.cpp, Groq, OpenAI…)
  and Anthropic's native API, with token streaming. Fully local or cloud — your
  choice per config.
- **Tools without the burden.** Built-in tools (`web_search`, `web_fetch`,
  `remember`, `recall`, `read_file`, `write_file`, `execute_shell`,
  `run_script`, `compress_context`, and meta-tools that scaffold new
  tools/agents from a conversation) run in-process — no protocol overhead. External
  [MCP](https://modelcontextprotocol.io) servers can be plugged in when you
  want more.
- **A workspace it can touch.** `read_file`/`write_file` are confined to one
  workspace directory; drag a file into the chat and its contents go straight
  into the model's context — including PDFs (text extracted automatically)
  and images (sent to the model as an actual image, if your LLM backend
  supports vision). `execute_shell` is real code execution and is off by
  default — see [Configuration](#configuration).
- **One front door, no picker.** You only ever talk to `funes`. It orchestrates:
  when a request needs a specialist (shell/file work, deep research, building a
  new tool or agent), it delegates via `delegate_to_agent` and relays the
  result in its own voice — you don't pick an agent, it does.
- **Agents as YAML** — really personas. Every "agent" is a name, a prompt, and
  a tool allowlist in `agents/*.yaml`, run by the same shared runtime — not
  independent autonomous entities. Delegation is what makes it a real (if
  simple) multi-agent setup rather than just a persona switch. Ship your own
  persona in five lines.
- **Pick up where you left off.** Every conversation with at least one message
  shows up in the conversations panel — click to switch back to it.
- **A household, not just you.** Each person gets their own account, and with
  it their own memories, conversations, scheduled jobs and files. Isolation is
  enforced in the SQL rather than in the handlers, so one account genuinely
  cannot see another's — not even by guessing an id. Accounts are created by
  an admin from the CLI or the first-run screen; there is no self-registration,
  because this is an appliance on your own network, not a service.

![Funes UI](docs/screenshot.png)

---

## Quick start

**Requirements:** Linux, CMake ≥ 3.14, a C++17 compiler, `libssl-dev`, `libyaml-cpp-dev`.
Everything else (SQLite, sqlite-vec, HTTP, JSON, MCP) is vendored. Optional:
`poppler-utils` (for `pdftotext`) if you want `read_file`/uploads to extract
text from PDFs — without it, PDFs just get a clear "not installed" error.

```bash
git clone https://github.com/Auto2ML/Funes.git
cd Funes
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**Point it at an LLM** — pick one:

```bash
# Option A: cloud (Anthropic) — create config/funes.local (gitignored):
cat > config/funes.local <<'EOF'
FUNES_LLM_URL=https://api.anthropic.com
FUNES_LLM_PROVIDER=anthropic
FUNES_LLM_MODEL=claude-haiku-4-5-20251001
FUNES_LLM_KEY=sk-ant-...
EOF

# Option B: fully local (llama.cpp) — the default config already points here:
llama-server -m qwen3-8b.gguf --port 8080 -c 8192
```

**Optional but recommended — semantic memory.** Any OpenAI-compatible
`/v1/embeddings` endpoint:

```bash
# local:
llama-server -m nomic-embed-text-v1.5.Q4_K_M.gguf --port 8081 --embedding
# or cloud (add to config/funes.local):
#   FUNES_EMBED_URL=https://api.openai.com
#   FUNES_EMBED_KEY=sk-...
#   FUNES_EMBED_MODEL=text-embedding-3-small
```

Without embeddings Funes still works — memory falls back to keyword search,
and missing vectors are backfilled automatically once an embedding endpoint
appears.

**Optional — MCP-based agents.** An agent can reach an external service
through an MCP server it spawns or connects to; nothing of the sort ships
here, because an MCP server is a credential and an account, not a feature of
the harness. See *External tools (MCP)* below for how to wire one up, and
[funes-julio](../funes-julio) for a worked example of a repository that adds
its own servers, agents and tools to this binary without forking it.

**Run it:**

```bash
./bin/funes
# → open http://localhost:8484
```

The first time you open it, Funes asks you to create the admin account —
there is no default password to forget to change. Everyone else gets an
account from you:

```bash
./bin/funes useradd marta --name "Marta"        # prompts for the password
./bin/funes userlist
```

Until that admin exists every API route answers `401`, which is why the
startup banner says so out loud rather than leaving you to wonder whether the
deployment is broken or merely unfinished.

---

## Users, and what "yours" means

Everyone who uses this Funes has an account, and everything Funes stores
belongs to exactly one of them: memories, conversation history, rolling
summaries, stored tool results, scheduled jobs, and workspace files.

The isolation is in the queries, not in the request handlers. Every storage
method takes the user it is acting for, and every statement carries a
`WHERE user_id = ?` — so a handler that forgets to check still cannot read
across accounts. Deletes are the same shape: `DELETE ... WHERE id = ? AND
user_id = ?` rather than a lookup followed by a check, which means
"that isn't yours" and "that doesn't exist" are indistinguishable from
outside and nobody can map the store by counting ids. Authentication itself
is checked twice on purpose — a gate that refuses any unauthenticated `/api/`
path, so a route added next year is protected by default, plus a per-handler
lookup that decides *whose* data to answer with.

Two front doors, one identity model:

- **The web UI** signs you in with a username and password and keeps an
  httpOnly session cookie. Passwords are stored as PBKDF2-HMAC-SHA256 — via
  OpenSSL, which Funes already links for HTTPS, so this cost no new
  dependency.
- **A non-browser caller** authenticates by service token plus an identity
  claim. A messaging bridge, say, sends `FUNES_SERVICE_TOKEN` proving the
  *caller* is trusted plus the sender's jid saying *who for*; Funes maps that
  jid to an account. Neither half authenticates anything on its own, and an
  unmapped jid is ignored. Map one with `funes jid-map <jid> <username>`.

Accounts are admin-managed by design — `funes useradd` / `userdel` /
`userlist` / `passwd`, with passwords always prompted rather than passed as
arguments, since argv is visible in `ps` and lands in your shell history.
There is no self-registration and no user-CRUD API, which keeps the
authenticated surface down to a single login endpoint. The one thing the
CLI refuses is deleting the last admin: first-run bootstrap only reopens when
there are *no* users at all, so an install with no admin can't be recovered
through any interface.

### What each person can do

Two roles. An **admin** can use every agent and every tool and manage
accounts; a **member** is scoped by an optional allowlist:

```bash
./bin/funes perms marta                              # what can she actually do?
./bin/funes perms marta --agents funes,researcher    # only these two
./bin/funes perms marta --deny web_fetch             # revoke one tool
./bin/funes perms marta --allow execute_shell        # grant a privileged one
./bin/funes perms marta --agents any --reset         # back to defaults
```

A member with nothing configured already has sensible limits: everything
ordinary is allowed, and the three tools that aren't — `execute_shell`,
`create_agent`, `create_tool` — are denied until you say otherwise. Shell
access is real code execution with the server's own rights; the other two
write into the install itself and outlive the conversation that asked for
them.

`run_script` is deliberately *not* in that privileged set: what it can start is
already limited to the scripts the agent was granted by name, and those are
programs an admin installed. Denying it (`--deny run_script`) still takes
scripts away from an account through every agent, and `--deny
run_script:backup_workspace` takes away exactly one — a script is a capability
in its own right, so the permission entry can name it.

Permissions only ever *restrict*. They're intersected with what the agent was
given, so granting someone `execute_shell` does not hand it to them through an
agent that never had it. And the agent allowlist covers delegation, not just
which agent you can open — otherwise it would be decorative, since `funes` is
reachable by everyone and would happily pass the task along on your behalf.

What stays shared: the LLM backend, the agent definitions in `agents/`, and
the credentials in `funes.local` — one search key, one mail account, one of
whatever else the install talks to. Per-user credential vaults are SaaS
territory and this is a household appliance.

Upgrading from 3.x is in-place. Existing memories, turns and files are
attributed to the admin account, and the vector index is rebuilt once with a
per-user partition — memory *texts* are never touched, and the embeddings
refill in the background.

---

## How memory works

Funes has three layers of memory, all in one SQLite file:

1. **Automatic recall.** Before every answer, the most relevant memories are
   retrieved by vector similarity and injected into the system prompt. The UI
   shows a *“Funes remembered N things”* chip you can expand.
2. **Deliberate memory.** The model has `remember` and `recall` tools and is
   prompted to quietly store facts worth keeping (preferences, projects,
   decisions) and to search its past before saying “I don't know”.
3. **Conversation history.** Each chat session's turns are stored and reloaded,
   so a page refresh doesn't lose the thread. Every session with at least one
   message is browsable from the **conversations panel** (*Chats* in the
   topbar) — a preview of the first message, last-active time, and a click
   to switch back to it. A delegated sub-agent's own task/answer isn't a
   separate turn here — only what you actually said and what `funes` actually
   answered.

Every memory is visible in the **memory panel**: source-tagged (`user` — you
taught it, `tool` — the model chose to keep it, `auto` — conversation log,
`consolidated` — merged from near-duplicates), searchable, and deletable. You
can also teach Funes facts directly from the panel. Forgetting is a feature:
the Borges story is a warning, after all.

And forgetting is now something Funes does on its own. Every few hours a
background pass **consolidates** the store: memories that say the same thing
(cosine ≥ 0.92) are handed to the model to merge into one sentence — or left
alone if it answers `KEEP ALL` — and `auto` memories older than 30 days that
were never once recalled are dropped. Explicit `user` memories are never
pruned, each merge is one transaction so a crash can't lose a fact, and the
whole thing is off with `FUNES_CONSOLIDATE=off`. Without an embedder the merge
step is skipped and only the prune runs. Each account's pool is consolidated
on its own: two people stating the same fact are two facts, and merging them
would write one person's wording into the other's memory.

### Large results don't eat the context window

A 40KB page fetch used to sit in the transcript for the rest of the turn,
costing ~10K tokens on every subsequent step. Now anything over 2KB is stored
once, out of band and scoped to the session, and the model sees a preview
instead — the size, the head and tail, and a `result_id`:

```json
{"result_id": 17, "tool": "web_fetch", "bytes": 41320,
 "head": "…", "tail": "…",
 "note": "Full result stored, not shown. Use read_result(id=17, offset, limit)…"}
```

`read_result` reads it back a window at a time, so a dereference can't
re-import what the preview evicted. Small results are untouched, so most turns
never notice. Delegation gets this for free: a specialist runs in the caller's
session, so a long report comes back to the orchestrator as a preview plus an
id it can read at its own pace.

---

## Configuration

Config is layered: shell env > `config/funes.local` (gitignored, secrets) >
`config/funes.conf` (committed defaults) > `~/.funes/config`.

| Variable | Default | Description |
|---|---|---|
| `FUNES_LLM_URL` | `http://localhost:8080` | LLM endpoint (OpenAI-compatible or Anthropic) |
| `FUNES_LLM_PROVIDER` | `openai` | `openai` \| `anthropic` |
| `FUNES_LLM_MODEL` | `default` | Model name |
| `FUNES_LLM_KEY` | *(empty)* | API key |
| `FUNES_EMBED_URL` | `http://localhost:8081` | Embeddings endpoint (unset → keyword memory) |
| `FUNES_EMBED_MODEL` | `default` | Embedding model name |
| `FUNES_EMBED_KEY` | *(empty)* | Embeddings API key |
| `FUNES_HOST` / `FUNES_PORT` | `127.0.0.1` / `8484` | Where the UI/API listens |
| `FUNES_DB` | `~/.funes/memory.db` | The memory file |
| `FUNES_DEFAULT_AGENT` | `funes` | Agent used when none is selected |
| `FUNES_MEMORY_RECALL_K` | `4` | Memories injected per answer |
| `FUNES_MEMORY_TURNS` | `10` | Past turns loaded per answer |
| `FUNES_AUTO_MEMORY` | `1` | Store each exchange as an `auto` memory |
| `FUNES_MCP_SERVERS` | *(empty)* | Extra MCP servers for every agent, `;`-separated URLs (SSE only — stdio servers are per-agent, see below) |
| `FUNES_ALLOW_LOCAL_FETCH` | `0` | Let `web_fetch` reach private/loopback hosts |
| `FUNES_WORKSPACE_DIR` | `~/.funes/workspace` | Sandbox root for `read_file`/`write_file`/`execute_shell` and uploads |
| `FUNES_ALLOW_SHELL` | `0` | Let `execute_shell` actually run commands (real code execution — see below) |
| `FUNES_SCRIPTS_DIR` | `./scriptlib` | The script library `run_script` starts scripts from, granted per agent by name (does *not* need `FUNES_ALLOW_SHELL`) |
| `FUNES_RESULT_TTL_DAYS` | `7` | Age at which stored tool results are swept at startup |
| `FUNES_CONSOLIDATE` | `on` | `off` disables the memory consolidation pass |
| `FUNES_CONSOLIDATE_HOURS` | `6` | How often consolidation runs |
| `FUNES_CONSOLIDATE_PRUNE_DAYS` | `30` | Age at which never-recalled `auto` memories are pruned |
| `FUNES_CONSOLIDATE_MAX_CLUSTERS` | `20` | Merge calls per run, so a backlog can't hog a local model |
| `FUNES_SERVICE_TOKEN` | *(empty)* | Shared secret for non-browser callers (a messaging bridge, a script). Unset = service authentication is off, not open. Generate with `openssl rand -hex 32` |
| `FUNES_COOKIE_SECURE` | `0` | Add `; Secure` to the session cookie. Leave off for plain-HTTP LAN use — the browser would refuse to store it and login would silently fail; set to `1` behind an HTTPS proxy |

Consolidation (after item 4 of NVIDIA's NOOA paper, arXiv 2607.20709)
targets *bloat*: it merges near-duplicate memories and
prunes stale never-recalled ones, but doesn't change how recall ranks what's
left. `recall_semantic` separately weights by `Memory::source` — a
deliberately taught `user`/`tool` fact outranks a passive `auto`
conversation-log entry at the same raw cosine similarity (`consolidated`
gets a smaller boost too), so one terse fact isn't crowded out of a small
top-k window by several longer, merely-similar transcripts that never
clustered close enough to merge. See `MemoryStore::source_weight` in
`src/core/memory.cpp`. The paper's fuller design — activation-ranked
injection, asynchronous reflection — is still just these two narrower
pieces in Funes, not the whole curation layer it describes.

The weight alone wasn't enough in practice: a query that's genuinely *about*
a topic ("¿Cual es mi nombre?") makes past conversations on that same topic
score high on raw similarity too, not just near-duplicate clutter — a modest
boost can't outrank a real semantic match, only a spurious one. Measured
against live scores, closing that gap took both a stronger weight (source
config in `memory.cpp`, not env-configurable) and more injected candidates
(`FUNES_MEMORY_RECALL_K`, raised from its code default of 4 to 6 in
`config/funes.conf`) so a correct-but-terse fact has more room to be one of
the ones that makes the cut, not just rank higher among four.

---

## Files, PDFs, images & shell

`read_file` and `write_file` are confined to the calling account's own
workspace, `FUNES_WORKSPACE_DIR/<user_id>/` — a path like `../secret`,
`/etc/passwd`, or `../3/notes.txt` is refused, whether or not it exists yet,
so the model can only ever touch that one directory. An agent's own
`workspace_dir` nests inside the caller's workspace, so a single agent
serving several people gives each of them a separate folder. Attach a file from the chat
UI (📎) and it's saved into the workspace, so a follow-up question can
reference it and `read_file`/`write_file` can act on it later. What happens
to the attachment depends on its type:

- **Text** (code, markdown, JSON, …) is inlined into your message as a
  fenced block.
- **PDF** has its text extracted (via `pdftotext`) and inlined the same way.
  Scanned/image-only PDFs extract no text and get a clear error instead.
- **Images** (PNG/JPEG/GIF/WEBP) are sent to the model as an actual image —
  a real multimodal message, not a text description. This only works if
  your LLM backend supports vision: cloud OpenAI/Anthropic models do; a
  local llama-server needs a vision-capable model *and* an `--mmproj` file
  loaded, or it'll reply with a plain error ("image input is not
  supported…") that Funes just relays rather than hides.
- Anything else is saved but not shown to the model.

`execute_shell` is different from all of the above: it's real, unconfined
code execution with the Funes process's own permissions — only its *working
directory* is the workspace, not its reach. It's **off by default**; set
`FUNES_ALLOW_SHELL=1` to turn it on, and only do that for a Funes instance
you trust with full access to your account. When enabled, each call still
gets a hard timeout (`timeout_seconds`, default 20s, max 120s) and a capped
output size.

The `funes` agent gets `read_file`/`write_file` by default. `operator`
(`agents/operator.yaml`) adds `execute_shell` on top, for when you want a
single agent dedicated to workspace/shell tasks.

### Scripts: the narrow version of that grant

Most of the time an agent doesn't need to run *anything*, it needs to run one
known thing — take a backup, report on the workspace, rebuild an index.
`execute_shell` is a poor fit for that: it hands over a command line the model
writes, so "let it take the backup" and "let it run `curl … | sh`" are the
same grant.

So scripts are declared the way tools are. `scriptlib/` (`FUNES_SCRIPTS_DIR`)
holds one manifest plus one executable file per script; an agent names the ones
it may run:

```yaml
tools:   [read_file, write_file, list_scripts, run_script]
scripts: [workspace_report, backup_workspace]      # empty or absent = none
```

The model picks a script by **name** and fills in the parameters the manifest
declares. Those become `--name=value` argv tokens passed straight to `execvp` —
there is no shell, so a value containing `;` or `$(…)` is one argument with
punctuation in it rather than a second command. Unknown parameters, missing
required ones and bad types are refused before the process starts.

A script can also declare the shape of what it prints, which the runtime then
checks — the same contract an `answer_schema` puts on an agent's final answer:

```yaml
output:
  format: json
  schema: {type: object, required: [archive, bytes]}
```

Exit code 0 is the script saying it worked; that block is Funes checking. A
script that exits 0 having printed the wrong shape fails the call, worded as an
installation fault, instead of handing the next step something it can't use.

Four consequences worth knowing:

- **`scripts:` denies by default**, unlike `tools:` where an empty list means
  everything. A file dropped into `scriptlib/` becomes runnable by nobody until
  an agent names it.
- **Budgets, contracts and permissions can name one script.**
  `tool_limits: {run_script:backup_workspace: 1}`,
  `require_tools: [run_script:backup_workspace]`, and
  `funes perms marta --deny run_script:backup_workspace` all work, because one
  tool standing in for several programs would otherwise share one ceiling, one
  contract slot and one grant between them.
- **`FUNES_ALLOW_SHELL` does not gate `run_script`** — including on a
  schedule: `schedule_job(kind="script")` puts a library script on a cron
  expression without shell access, and re-checks both the agent's grant and
  the account's permissions when the job fires, so revoking either stops the
  timer. That's the point: a reviewed program, installed by an admin and
  granted to one agent by name, is a different act from an arbitrary command
  line. An install that needs neither can leave shell off entirely.
- **The library sits outside every workspace**, so `read_file`, `write_file`
  and the upload endpoint cannot read a script, edit one, or add one.
  Installing a script is an admin editing the repo, like adding an agent.

It is an allowlist, not a sandbox: a script runs with the Funes process's own
permissions, so review one before installing it exactly as you'd review an
agent prompt — and don't install one that takes a command, a path outside the
workspace, or a URL to fetch and run. See
[`scriptlib/README.md`](scriptlib/README.md) for the manifest format.

---

## Agents (and why that word is doing some work)

```yaml
# agents/researcher.yaml
name: researcher
description: Digs into a topic on the web, then remembers what it learned.
tools: [web_search, web_fetch, remember, recall]
max_steps: 12
system_prompt: |
  You are a research assistant with persistent memory. ...
```

Honestly: an "agent" here is a name, a prompt, and a tool allowlist, run by
the one shared tool-calling loop — closer to a persona/config profile than an
independent autonomous agent. Each has its own memory namespace (`remember`/
`recall` are scoped by agent name), and each can be talked to directly via
the API (`{"agent": "researcher", ...}` on `/api/chat`) — but the UI only
ever talks to `funes`, which delegates to the others through
`delegate_to_agent` rather than making you switch. That delegation is what
turns "a few personas" into something closer to actual multi-agent
orchestration: `funes` hands a specialist a task, gets back an answer, and
relays it — the specialist's own tool calls stay hidden, only `funes`'s
final message and one expandable chip show up in the chat.

Drop a file in `agents/`, hit *reload* (`POST /api/agents/reload`), and
it's immediately delegatable — no restart needed (unlike a new *tool*,
which needs a rebuild — see "Files, PDFs, images & shell" below).
`agent-builder` (delegate a request like "design an agent that...") does
this for you: it interviews you, drafts the prompt, and calls `create_agent`
to write and reload the YAML live.

### Making an agent finish what it started

A local model on a long multi-step task will often narrate the last tool
result as if it were the deliverable and stop — reporting "sent!" without
having called the thing that sends. The loop used to accept that, because
any message without tool calls looked like a final answer. `require_tools`
closes it:

```yaml
require_tools: [write_file, execute_shell]
```

Those tools must have *succeeded* before a text answer counts as final.
Try to finish early and you get a nudge naming what's outstanding (with
`tool_choice` forced to `required` for that turn, so it can't reply with
more prose); refuse long enough and the run returns an explicit `FAILED —
...` instead of a plausible-sounding lie. Worth setting on any agent whose
real output is a side effect rather than its text.

### Making an agent stop researching

The mirror image. `require_tools` is a floor — these calls must happen;
`tool_limits` is a ceiling — this call may not happen more than N times:

```yaml
tool_limits:
  web_search: 6
```

A research agent will otherwise happily search forever: ours once issued 20
distinct `web_search` calls in a single run without ever synthesizing, twice
in the same pipeline, and the loop detector killed both runs after half an
hour of GPU. Past the ceiling the call is *refused* rather than executed, and
the model is told to conclude from what it already has — so the run still
produces an answer. That's the difference from the loop detector, which stops
the run without one and stays as the backstop.

Being told to stop isn't enough on its own: a 9B ignored the refusal and
re-issued the same call seven times in a row, burning a step each time until
the run died with nothing. So the turn straight after a refusal is offered no
tools at all, which leaves answering as the only move available.

`0` forbids a tool for the run; a tool with no entry is unlimited.

The same trick covers the case a ceiling can't reach. An uncapped tool is
never refused, so a model that keeps finding one more thing to look up walks
off the end of `max_steps` with a full history and nothing said — ours spent
all 20 steps on `read_result` and returned a failure with no sources in it.
So the final step is always offered no tools: whatever the run has gathered
by then, that step is spent writing it up. It is a forced synthesis, not a
salvage — the model writes the answer itself, and a step that still produces
nothing fails as loudly as before.

Withholding means the tool schema leaves the prompt entirely, not just that
the request says not to use it. Both are worth knowing about if you are
reading the wire: a model that can still see a tool it has been forbidden
reaches for it anyway, and since the server emits no call, it writes one out
as prose instead. Funes normally rescues those — local models often can't
manage a native call — but a rescued call on a withheld turn hands back
exactly the move that was being taken away, so there it is dropped instead.

### Making an agent answer in a shape you can use

`require_tools` checks that the work happened; it says nothing about what comes
back. For an agent whose answer feeds something else — another agent, a
pipeline, a script — declare the shape:

```yaml
answer_schema:
  type: object
  required: [summary, sources]
  properties:
    summary: { type: string }
    sources: { type: array, items: { type: string }, minItems: 1 }
```

The matching "your final answer must be…" instruction is generated from this
at load, so the prompt can't drift from what's enforced. An answer that
doesn't validate gets a nudge naming the exact violation (`sources[1]:
expected string, got number`); the answer that does validate is returned as
canonical JSON, fence and preamble stripped. Same nudge budget as
`require_tools`, and tool nudges go first — side effects before formatting.
Spend the budget and the run returns `FAILED — …`, including from the
loop-detector and `max_steps` bailouts, which can't satisfy a schema and
shouldn't pretend to. Supported keywords are `type`, `required`,
`properties`, `items`, `enum`, `minItems`/`maxItems`; anything else in the
schema is ignored rather than rejected.

### Not asking the model for what a tool can supply

The three features above make a model finish, stop and answer correctly. The
cheapest way to stop it getting a step wrong is not to ask it. The worked
example — a daily newsletter that went from three agents and 46 steps to one
agent and about four, by moving search, dedup, fetch, link-check, render and
send into two tools and leaving the model only the judgement — lives with the
newsletter itself, in the [funes-julio](../funes-julio) extension (its README
carries the full incident write-up). The rule it left behind is general:

- **A URL is never retyped.** A tool hands the model a numbered pool; the model
  picks by number; a tool resolves the number back. What the model never saw a
  reason to copy, it cannot copy wrong.
- **A claim is checked, not trusted.** Where the model's judgement has a
  checkable half ("this item is about X" → a quote from the page that says so),
  a tool checks it against the text actually fetched.
- **An exit code is a fact; a sentence is a claim.** "It was sent" is the send
  script's return value, recorded by the tool, never the model's summary.
- **Convention is configuration.** A stage's directory, filename and JSON shape
  are declared once and enforced by the runtime, not restated in five prompts.
  Arithmetic (a Borda count over three rankings) is a tool, not a prompt.

The pieces of the core that carry this rule — `answer_schema`, `require_tools`,
`tool_limits`, `scriptlib`'s `output:` contract, `NativeTool::inline_result` —
stayed. The tools that first needed it (`harvest_candidates`, `publish_issue`,
`write_structured`/`read_structured`, `merge_rankings`) moved out as the first
extension; see "Extensions" below for how they plug back in.

### Extensions: native tools that live somewhere else

A tool is C++ and needs a rebuild to add, which is right for the generic set
and wrong for one operator's workflow. An extension is a directory outside
this tree with an `extension.cmake` and self-registering sources
(`src/core/extension.h`):

```bash
cmake -B build -DFUNES_EXTENSIONS=/path/to/my-extension
cmake --build build -j$(nproc)
FUNES_AGENTS_DIR=./agents:/path/to/my-extension/agents ./bin/funes
```

The extension's tools register beside the built-ins with the same registry,
memory store and workspace root, and nothing else; its agents load from a
second `agents/` directory (`FUNES_AGENTS_DIR` is colon-separated, later
entries shadow earlier ones by name); its configuration is its own. A core
build with no extension has seven agents and no publishing pipeline, and every
test still passes.

To give an agent tools from an external MCP server, over HTTP+SSE:

```yaml
mcp_servers:
  - url: http://localhost:9000
    name: my-tools
```

or over stdio — Funes spawns the command as a subprocess and speaks MCP over
its stdin/stdout, which is the transport most published MCP servers actually
use:

```yaml
mcp_servers:
  - command: npx -y rss-reader-mcp
    name: rss-reader-mcp
    env:
      SOME_API_KEY: value   # optional, passed to the subprocess's environment
```

The example above is a real third-party server
([rss-reader-mcp](https://www.npmjs.com/package/rss-reader-mcp),
`fetch_feed_entries`/`fetch_article_content`); drop those four lines into an
agent's YAML and its tools appear alongside the built-ins. Every MCP
server, SSE or stdio, is reconnected fresh per agent instance (i.e. per
request) — for a stdio server that means a new subprocess each time, so
expect its startup cost (e.g. `npx`'s package resolution) on every call, not
just the first.

---

## API

Everything the UI does is plain HTTP — script it if you like:

Every route below needs a signed-in user except the four marked *public*.

```
POST   /api/login                     {username, password} → session cookie   (public)
POST   /api/logout                    revoke the token, clear the cookie
GET    /api/auth/status               {needs_bootstrap, authenticated, user?}  (public)
POST   /api/auth/bootstrap            create the first admin — refused once
                                       any user exists                         (public)
GET    /api/status                    health + model + memory stats
GET    /api/agents                    available agents
POST   /api/agents/reload             re-read agents/*.yaml
POST   /api/chat                      {agent?, session, message?, images?} → SSE stream
GET    /api/memories?agent=&q=        list / semantic search
POST   /api/memories                  {text, agent?} — teach a fact
DELETE /api/memories/<id>             forget
GET    /api/history?session=          a session's turns
GET    /api/sessions?limit=           conversation list: preview + last activity + turn count
GET    /api/jobs                      scheduled cron jobs (admin: ?all=1 for every account's)
GET    /api/files?path=              list files in the user's workspace
GET    /api/files/download?path=     download a workspace file
DELETE /api/files?path=              delete a workspace file or folder
POST   /api/upload                   multipart 'file' → saved to the workspace, plus a text
                                       preview (text/PDF) or base64 (image) for the UI to send on
POST   /api/upload-batch             multipart 'file' (multiple) + optional 'folder' → saves
                                       files to a named subfolder, returns a manifest
```

Everything except the public routes above is scoped to the signed-in user:
`/api/memories`, `/api/sessions`, `/api/history` and `/api/jobs` answer with
that account's rows and no one else's, and `DELETE /api/memories/<id>` on
somebody else's memory is a `404`.

`images` is an array of `{mime_type, data}` (base64, no `data:` prefix), max 4
per message — the same shape `/api/upload` hands back for an image file. The
chat stream emits `memories`, `delta`, `tool_call`, `tool_result`,
`result_stored`, `contract_nudge`, `schema_nudge`, `context_compressed`,
`usage`, `done`, and `error` SSE events.

---

## Tests

```bash
cd build && ctest --output-on-failure   # unit tests (memory, users, tools, config, publishing)
bash tests/integration.sh               # end-to-end against a mock LLM, no network
```

`ctest` runs `publishing/`'s Python suite too, via the same `--self-test` flag
that runs it on the machine that sends the mail — one set of assertions rather
than a CI copy and a deployed copy that drift.

`test_user_isolation` is the suite worth watching: it asserts that no account
can read, delete or overwrite another's memories, turns, summaries, stored
results, scheduled jobs or files. It also checks something that isn't a leak
at all — that a user still gets their own recall results when another
account's pool is two hundred times larger. That one guards the vector
index's per-user partitioning, whose absence wouldn't expose anything, just
quietly make recall worse for everyone as accounts were added.

---

## Project structure

```
Funes/
├── agents/            # the seven shipped agents (funes, researcher, operator, file-reviewer,
│                      # agent-builder, tool-builder, agent-doctor)
├── config/            # funes.conf (defaults) + funes.local (secrets, gitignored)
├── src/
│   ├── core/          # llm_client (+ multimodal messages), memory, users + password
│   │   │              # hashing, tools, agent runtime, context compression,
│   │   │              # completion contract + answer schema, result store,
│   │   │              # base64, UTF-8-safety helpers
│   │   └── tools/     # web_search/fetch, remember/recall, read_result, read/write_file
│   │                  # (+ PDF extraction), execute_shell, list_scripts/run_script,
│   │                  # compress_context, schedule_job & co.,
│   │                  # create_tool/create_agent, delegate_to_agent
│   │                  # (+ generated/, self-registering)
│   └── server/        # HTTP API + SSE + entry point + the admin user CLI
├── scriptlib/         # the script library: what an agent may run by name, not by command
├── ui/                # web UI (vanilla JS — no build step)
├── tests/             # unit tests + mock-LLM integration test
└── third-party/       # vendored: sqlite, sqlite-vec, cpp-mcp (httplib, json)

One operator's own agents, tools and pipelines — a newsletter, a debate
pipeline, WhatsApp, Gmail, a manuscript editor — live in a separate
repository built in as an extension, not here. See "Extensions" above.
```

## Lineage

Funes is the convergence of a series of experiments in giving LLMs memory and
tools: the original Python Funes (pgvector + Gradio, 2024), AgentOS/NeuralOS
(C++ agent runtimes), and [AresOS](https://github.com/Auto2ML) (an agent-native
OS layer for Linux, where most of this backend was born). This repo keeps the
useful 20% of all of that — an assistant that remembers — without the burden.

## License

MIT — see [LICENSE](LICENSE).
