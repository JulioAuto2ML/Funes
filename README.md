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
  `compress_context`, and meta-tools that scaffold new tools/agents from a
  conversation) run in-process — no protocol overhead. External
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
- **WhatsApp** authenticates by number. The autoresponder sends a service
  token proving the *caller* is trusted plus the sender's jid saying *who
  for*; Funes maps that jid to an account. Neither half authenticates
  anything on its own, and an unmapped number is ignored exactly as it was
  before. Map one with `funes jid-map <jid> <username>`.

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

Permissions only ever *restrict*. They're intersected with what the agent was
given, so granting someone `execute_shell` does not hand it to them through an
agent that never had it. And the agent allowlist covers delegation, not just
which agent you can open — otherwise it would be decorative, since `funes` is
reachable by everyone and would happily pass the task along on your behalf.

What stays shared: the LLM backend, the agent definitions in `agents/`, and
the credentials in `funes.local` (one Gmail account, one Tavily key, one
WhatsApp bridge). Per-user credential vaults are SaaS territory and this is a
household appliance.

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
| `FUNES_RESULT_TTL_DAYS` | `7` | Age at which stored tool results are swept at startup |
| `FUNES_CONSOLIDATE` | `on` | `off` disables the memory consolidation pass |
| `FUNES_CONSOLIDATE_HOURS` | `6` | How often consolidation runs |
| `FUNES_CONSOLIDATE_PRUNE_DAYS` | `30` | Age at which never-recalled `auto` memories are pruned |
| `FUNES_CONSOLIDATE_MAX_CLUSTERS` | `20` | Merge calls per run, so a backlog can't hog a local model |
| `FUNES_SERVICE_TOKEN` | *(empty)* | Shared secret for non-browser callers (the WhatsApp autoresponder). Unset = service authentication is off, not open. Generate with `openssl rand -hex 32` |
| `FUNES_COOKIE_SECURE` | `0` | Add `; Secure` to the session cookie. Leave off for plain-HTTP LAN use — the browser would refuse to store it and login would silently fail; set to `1` behind an HTTPS proxy |

Consolidation (`docs/nooa-comparison.md` item 4, from NVIDIA's NOOA paper —
arXiv 2607.20709) targets *bloat*: it merges near-duplicate memories and
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
cheapest way to stop it getting a step wrong is not to ask it, and the
`curator` agent is what that looks like carried all the way.

The daily newsletter used to be three agents and 46 steps, most of them a
language model reading instructions about how to perform a deterministic task.
One of those steps was retyping ten URLs it had read earlier, out of a
transcript that had since been compressed and stored by reference. On
2026-07-31 an item about an OpenAI breach shipped pointing at a Stripe checkout
page — which returned 200, so the link checker passed it. That is arithmetic,
not carelessness: ask for ten transcriptions a day and one comes out wrong every
few weeks, forever.

So `harvest_candidates` runs the searches, drops duplicates, stale items and
anything that already ran this week, fetches every survivor (a failed fetch
drops the candidate *there*, which is the link check moved before selection
instead of after writing), and returns a numbered pool. The model picks by
number. `publish_issue` resolves those numbers back to URLs it never showed the
model a reason to retype, and renders, re-checks and sends in one call whose
exit code is what makes "it was sent" a fact.

The one thing a model cannot be relieved of is whether an item is worth running
and whether the post is true. The second half of that is checkable, so it is
checked: each item carries an `evidence` quote copied off the candidate's page,
verified against the text the harvester actually fetched. A post about a breach
cannot produce a supporting quote from a checkout page. A failed check names one
item and sends nothing.

Three agents and 46 steps became one agent and about four.

The same split decides what is configuration. A publication is one YAML in
`publications/` — queries, recency window, caps, artifacts, channels — plus a
prose voice file, because voice is the one part a model has to read rather than
obey. The agent never sees the config; it is handed a pool and a voice note by
the tools, which is why a second publication needs no second agent. Every run
leaves a record in `runs/<publication>/<date>.json`, and both the scheduler and
the LinkedIn cron read it rather than believing anything the model said.

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

`agents/rss-reader.yaml` wires this up end to end against a real third-party
server ([rss-reader-mcp](https://www.npmjs.com/package/rss-reader-mcp),
`fetch_feed_entries`/`fetch_article_content`) as a working example. Every MCP
server, SSE or stdio, is reconnected fresh per agent instance (i.e. per
request) — for a stdio server that means a new subprocess each time, so
expect its startup cost (e.g. `npx`'s package resolution) on every call, not
just the first.

`agents/gmail-assistant.yaml` is a second example, wired to
[imap-email-mcp](https://www.npmjs.com/package/imap-email-mcp) over IMAP/SMTP
rather than the Gmail API — no OAuth app to register, it authenticates with a
plain Gmail App Password. Its `tools:` list only allows the read/search/draft
tools (`search_emails`, `list_emails`, `get_email`, `list_folders`,
`list_drafts`, `get_draft`, `create_draft`, `update_draft`); the server also
exposes `send_email` and `delete_email`, but leaving them out of `tools:` is
enough to keep the agent from ever calling them — Funes filters the tool
schema handed to the model down to that list (`agent.cpp`'s
`tools_.openai_schema(cfg_.tools)`), so unlisted tools are invisible to it,
not just discouraged. Note what's *not* in the yaml's `env:` block: only
`IMAP_HOST` is set there, because it isn't secret. `IMAP_USER` and
`IMAP_PASSWORD` are deliberately absent from the file — a stdio server's
subprocess inherits Funes' own process environment (that's how
`FUNES_MCP_SERVERS` reaches agents too), so the credentials just need to live
in `config/funes.local` (same Gmail address and App Password
`publishing/send_newsletter.py` uses — see the Email section in
`config/funes.conf`). Never put a real secret in an agent yaml's `env:` map —
anything written there is committed to the repo in plain text.

The `command:` doesn't run `npx -y imap-email-mcp` directly, though — it
points at `third-party/imap-email-mcp-patched/node_modules/imap-email-mcp/index.js`,
a pinned local install. The published package's `node-imap` dependency never
sets the TLS SNI `servername` when connecting, and Gmail's IMAP frontend
responds to a connection with no SNI by handing back a fallback certificate
that's genuinely self-signed — every request would fail with `Error:
self-signed certificate` otherwise. `third-party/imap-email-mcp-patched/`
pins the exact version and carries a one-line patch (via
[patch-package](https://www.npmjs.com/package/patch-package), see
`patches/imap+0.8.19.patch`) that sets `servername` explicitly. Run `npm
install` in that directory once per host (local dev and yoda both) before
this agent can connect — `patch-package` reapplies the patch automatically
on every install, so it survives a clean `node_modules` wipe.

`agents/whatsapp-assistant.yaml` is a third example, wired to
[whatsapp-mcp](https://github.com/lharries/whatsapp-mcp) — a personal
WhatsApp account connected the same way WhatsApp Web/Desktop links a device
(scan a QR code with the phone once), via the unofficial `whatsmeow`
protocol library. Unlike the other two, it's a two-process design vendored
in full under `third-party/whatsapp-mcp/`:

- **The Go bridge** (`whatsapp-bridge/`) holds the actual WhatsApp
  connection and writes incoming messages to a local SQLite store. It is
  **not** spawned by Funes — Funes only starts short-lived stdio
  subprocesses per request, but this bridge has to stay connected
  continuously to receive messages at all. Run it as its own systemd
  `--user` service; see `scripts/whatsapp-bridge.service` for the unit file
  and setup steps (including the one-time QR scan, and re-pairing roughly
  every 20 days when the session expires).
- **The Python MCP server** (`whatsapp-mcp-server/`) is the thin tool layer
  Funes actually spawns per request (`uv --directory
  third-party/whatsapp-mcp/whatsapp-mcp-server run main.py`) — it just reads
  the bridge's SQLite DB and calls its local REST API.

Locally patched, three times. The bridge's REST API was hardcoded to port
8080, which collides with yoda's `llama-server` (`FUNES_LLM_URL`); both
`whatsapp-bridge/main.go` and `whatsapp-mcp-server/whatsapp.py` were edited
to use 8090 instead. Separately, the vendored `whatsmeow` dependency was
~17 months stale as of first deploy and got rejected outright ("Client
outdated (405)") — `go get -u go.mau.fi/whatsmeow@latest` fixed that but
changed several method signatures to take a leading `context.Context`
(`client.Download`, `sqlstore.New`, `container.GetFirstDevice`,
`client.GetGroupInfo`, `client.Store.Contacts.GetContact`), all updated in
`main.go` to pass `context.Background()`. Expect to need this again
periodically — it's an unofficial protocol implementation racing WhatsApp's
actual client version. The agent's `tools:` list deliberately excludes
`send_file`, `send_audio_message`, and `download_media` — it can search,
read, and send plain-text replies, nothing else. Because WhatsApp message
content is untrusted input the model reads directly, the system prompt
tells it explicitly not to treat message text as instructions.

**Two numbers, two bridge instances.** `whatsapp-assistant` acts as *you* —
it should stay on your own number. `whatsapp-autoresponder` talks back
autonomously to whoever's on the whitelist, which is a materially different
thing to hand your personal WhatsApp identity to, so it gets a second,
dedicated number instead. The third patch — `WHATSAPP_STORE_DIR` /
`WHATSAPP_BRIDGE_PORT` env vars read once at package init in `main.go` —
turns every hardcoded `"store"` path and the port literal into per-instance
config, so the same compiled binary runs twice with independent sessions and
SQLite stores: `whatsapp-bridge.service` (personal, port 8090, `store/`) and
`whatsapp-bridge-funes.service` (dedicated, port 8091, `store-funes/`, env
set in the unit file). Pairing the second instance needs an actual second
phone number you control — that's on you to provide, this only handles the
bridge process once you have one. `whatsapp-mcp-server` (whatsapp-assistant's
MCP layer) still only ever points at the personal instance;
`whatsapp_autoresponder.py` points at the dedicated one via
`WHATSAPP_DB_PATH`/`WHATSAPP_BRIDGE_URL` in `config/funes.conf`.

**Auto-replying to incoming messages** is a separate, opt-in layer on top of
the above — `agents/whatsapp-assistant.yaml` only answers when *you* ask
Funes (through its own UI) to check or send WhatsApp; it does nothing when a
message just arrives. `scripts/whatsapp_autoresponder.py` is a small
standalone poller (stdlib-only, no new dependency) that watches the
dedicated instance's SQLite store directly and, for messages from chats on
`WHATSAPP_WHITELIST` only, asks a dedicated `whatsapp-autoresponder` agent
for a reply and sends it back. Every other chat is silently ignored.
Manage the whitelist with `scripts/whatsapp_whitelist.py list/add/remove`
(matches by contact name, so you don't have to hand-look-up a `jid`) rather
than editing `config/funes.local` directly — see the WhatsApp section of
`config/funes.conf` for the commands. It's a plain local CLI, not something
exposed to any agent: changing who Funes will auto-reply to should stay a
step only you can take.

The autoresponder agent is deliberately more restricted than
`whatsapp-assistant`: its only tools are `recall`/`remember` (memory), no
`delegate_to_agent`, no MCP servers, and specifically **no send capability**
— it can only return text. `whatsapp_autoresponder.py` is the one thing that
actually calls the bridge's `/api/send`, and it always sends into the exact
chat the incoming message came from, for whitelisted chats only. That split
exists so the model can never pick who to send to; it only ever picks what
to say, and even that only reaches a chat that already passed the whitelist
check in plain Python before the model saw anything. Run it as its own
systemd `--user` service — see `scripts/whatsapp-autoresponder.service`.

It does, though, share `funes`'s own memory rather than starting a separate,
empty pool — via `memory_scope: funes` in its yaml. Every agent normally has
fully isolated memory (`recall`/`remember` are scoped by agent name — see
`AgentConfig::memory_scope` in `src/core/agent_config.h`); this is one field
away from opting out of that isolation for a specific agent, so a message
sent over WhatsApp and one sent through the web UI draw on and add to the
same facts, rather than the WhatsApp side starting from a blank slate.

**Starting a fresh conversation.** A contact can send `/new` to reset their
thread. This is handled entirely in `whatsapp_autoresponder.py` — the model
never sees it — and, like the web UI's own "New Chat" button (`ui/app.js`),
it rotates to a new session rather than deleting the old one's history
(`sanitize_session`'s `generation` suffix, tracked per `chat_jid` in the
poller's own state file). Note this only resets *conversation* history —
`recall`/`remember` long-term memory is shared with `funes` (see above) and
is unaffected either way. There's currently no way to actually delete a
session's history — through WhatsApp or the web UI — only start a new one;
the storage layer has the pieces (`MemoryStore::prune_turns`) but nothing
exposes them yet.

**Documents and photos over WhatsApp.** A whitelisted contact can send a
"document" attachment (PDF or plain text file) or a photo ("image"
attachment) and Funes will read it — audio/video are still ignored, that
would need transcription, a separate feature. `whatsapp_autoresponder.py`
downloads it via the bridge's `/api/download` (pre-checking the message's
known `file_length` against `WHATSAPP_MAX_MEDIA_BYTES` before downloading,
and re-checking the actual size after, so an oversized file is never handed
to the model), copies it into the *sender's own* workspace under a per-chat
subfolder, and tells the agent about it with a `[Document received: <path>]`
or `[Photo received: <path>]` marker in the message text. Which workspace
that is comes from Funes, not from the script: the poller asks which account
the number maps to and writes there, so identity is resolved in one place and
an unmapped number's attachments are ignored rather than written somewhere
nothing can read them. `whatsapp-autoresponder`'s only new tool for this is
`read_file`, scoped via `workspace_dir: whatsapp-uploads` in its yaml, which
resolves to `<workspace>/<user_id>/whatsapp-uploads/` — the same confinement
`read_file` gives every other agent (see `src/core/tools/fs_guard.h`), so it
can never reach anything outside that one folder, including another
contact's. `read_file` already knows
how to pull text out of a PDF (`src/core/tools/pdf_extract.cpp`, the same
code path the web UI's drag-and-drop upload uses) and to hand a PNG/JPEG/
GIF/WebP image back as multimodal content (`funes::detect_image_mime`,
`src/core/tools/file_tools.cpp`) for a vision-capable backend to read —
Funes' own deployment uses a Qwen model with an `--mmproj` file loaded for
this (see `FUNES_LLM_URL`/the llama-server setup); without a vision-capable
backend, images are silently ignored by the model the same way they'd be by
a text-only one. Anything read_file still can't handle (spreadsheets, Word
docs) comes back as a plain error the model is told to relay honestly
rather than bluff through. Uploads are deleted automatically once they're
older than `WHATSAPP_UPLOAD_MAX_AGE_DAYS` (default 30) — see the WhatsApp section
of `config/funes.conf`.

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
POST   /api/upload                    multipart 'file' → saved to the workspace, plus a text
                                       preview (text/PDF) or base64 (image) for the UI to send on
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
├── agents/            # agent definitions (funes, researcher, operator, tool-builder, agent-builder…)
├── config/            # funes.conf (defaults) + funes.local (secrets, gitignored)
├── src/
│   ├── core/          # llm_client (+ multimodal messages), memory, users + password
│   │   │              # hashing, tools, agent runtime, context compression,
│   │   │              # completion contract + answer schema, result store,
│   │   │              # base64, UTF-8-safety helpers
│   │   └── tools/     # web_search/fetch, remember/recall, read_result, read/write_file
│   │                  # (+ PDF extraction), execute_shell, compress_context,
│   │                  # create_tool/create_agent, delegate_to_agent,
│   │                  # harvest_candidates/publish_issue
│   │                  # (+ generated/, self-registering)
│   └── server/        # HTTP API + SSE + entry point + the admin user CLI
├── publications/      # one YAML + one voice file per publication
├── publishing/        # the scripts that render, send and post an issue (Python)
├── ui/                # web UI (vanilla JS — no build step)
├── tests/             # unit tests + mock-LLM integration test
└── third-party/       # vendored: sqlite, sqlite-vec, cpp-mcp (httplib, json)
```

## Lineage

Funes is the convergence of a series of experiments in giving LLMs memory and
tools: the original Python Funes (pgvector + Gradio, 2024), AgentOS/NeuralOS
(C++ agent runtimes), and [AresOS](https://github.com/Auto2ML) (an agent-native
OS layer for Linux, where most of this backend was born). This repo keeps the
useful 20% of all of that — an assistant that remembers — without the burden.

## License

MIT — see [LICENSE](LICENSE).
