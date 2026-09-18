# Generalizing Funes: from incident-driven specifics to reusable primitives

## Diagnosis

The shared runtime (`agent.cpp`, `tools.h/cpp`, the safety-mechanism stack) is
already generic — one loop executes all 17 `agents/*.yaml` identically, and
`agents/README.md`/`src/core/README.md` show that most of the hardening
(`tool_budget`, `completion_contract`, loop detection) was built as reusable
infrastructure, not per-agent hacks. The overfitting is one level up: in how
individual **agents and pipelines** were added on top of that runtime. Four
concrete patterns repeat across the codebase.

### 1. Two solutions to the same problem, one generalized and one not

`curator` + `harvest_candidates`/`publish_issue` + `publications/*.yaml` is
the newsletter pipeline, and it is already the model for what "generalized"
looks like here: it replaced a 3-agent, 46-step design that failed in
production (an LLM retyped a URL from memory and shipped a broken link — see
root `README.md`). The fix was a rule, not a patch: **don't ask the model for
what a tool can supply.** Story selection and post text stayed with the LLM;
URL resolution, link verification, and content-grounding checks became
deterministic tool code. A new publication today is a YAML file plus a voice
note — no new agent.

`voc-researcher` → `council-chair` → `council-panelist` (×3) →
`content-writer` → `mvp-builder` is a second, structurally identical
pipeline — gather → judge/select → produce → (optionally) build — built
*after* the newsletter rewrite, but without absorbing its lesson:

- File conventions (`topics/<date>-<slug>.json`, `debates/<date>-<slug>.json`,
  `articles/<slug>/draft.md`) are prose in system prompts, not a schema the
  runtime enforces. Nothing stops a slug drifting or a required field being
  dropped, the way `answer_schema` stops it for `council-panelist`'s own
  output.
- `council-chair`'s system prompt asks the model to *compute a Borda count by
  hand* to merge three panelists' rankings — deterministic arithmetic,
  currently done by an LLM, in the same codebase that has already
  demonstrated (`publish_issue`'s word-overlap grounding check) that this
  class of check belongs in a tool.
- There is no equivalent of `publish_issue`'s grounding check: nothing
  verifies a `content-writer` claim against the sources `web_fetch` actually
  returned, even though the newsletter pipeline already carries the pattern
  (`evidence` quote verified word-for-word) for exactly this problem.

### 2. Single-project agents standing in for missing generic capability

`book-editor` hardcodes one author's full Chicago Manual of Style ruleset and
one workspace (`book`) into its system prompt. `astro-ph-summarizer` is a
near-duplicate of `rss-reader` (same tool pair: `fetch_feed_entries`,
`fetch_article_content`) scoped to exactly one arXiv feed. Both are personal
configurations promoted to permanent agent definitions, in a system that
otherwise externalizes configuration (`publications/*.yaml`,
`agents/*.yaml` itself).

### 3. Three different shapes for "watch an external source, act as Funes"

- **MCP pull** (`gmail-assistant`, `rss-reader`): the agent calls out via an
  MCP server when asked. No polling, no identity problem — the human is
  already in the session.
- **MCP push via bridge, interactive** (`whatsapp-assistant`): same shape,
  different transport.
- **Bespoke standalone poller** (`scripts/whatsapp_autoresponder.py`, 550
  lines): polls a SQLite store, resolves identity via jid-map, calls
  `/api/chat` with a service token, sends the reply back through a REST call
  hardcoded to the source chat. This shape — poll external source → resolve
  identity → call Funes → deliver reply — is real, reusable infrastructure
  (state-file watermarking, upload handling, expiry sweep are all
  non-trivial and channel-agnostic) but it exists only inside one script.
  A fourth channel (Telegram, Slack, Discord) means writing a second
  550-line poller from scratch, including re-solving the two documented
  gotchas (`WHATSAPP_STATE_PATH`, `WHATSAPP_DB_PATH` collisions between two
  installs) that have nothing to do with WhatsApp specifically.

### 4. Self-service is asymmetric

`create_agent` (`agent-builder`) hot-reloads — no rebuild, no human in the
loop beyond the interview. `create_tool` (`tool-builder`) scaffolds a C++
source file that needs `cmake --build` and a restart, and is explicitly
scoped to "one HTTP call" per its own system prompt — anything else is
out-of-scope. But `mcp_servers:` in agent YAML is *already* the more general,
already-hot-reloadable integration point (stdio or SSE, arbitrary tool
surface, used by 5 of 17 agents today) — nobody who wants a new integration
is being pointed at the path that doesn't need a human running a build.

## The principle to generalize

Everything above is one instance of the same fix, applied once
(newsletter) and not yet propagated:

> **Config over prose, tools over model arithmetic, for anything with a
> checkable right answer.** The model should decide the things that need
> judgment (which story, which chapter fix, which channel to build) and
> nothing else — file paths, schema shape, scoring, and verification belong
> in code the runtime enforces.

## Plan

### Phase 1 — Generic pipeline primitive

Extract the shape behind `harvest_candidates`/`publish_issue` into something
usable outside publishing:

- A schema-validated `write_structured`/`read_structured` tool pair, backed
  by a `pipelines/*.yaml` config (stage name → directory → JSON schema),
  mirroring how `publications/*.yaml` already drives `curator`. A pipeline
  stage's schema violation should be rejected by the tool with a specific
  error, the same way `publish_issue` rejects a >280-char post — not
  silently written as malformed JSON.
- A deterministic `merge_rankings` tool (Borda count or similar) so
  `council-chair` calls a tool instead of doing arithmetic in-context.
- Retrofit `voc-researcher`, `council-chair`, `council-panelist`,
  `content-writer`, `mvp-builder` onto both. This is the highest-value,
  lowest-risk phase: it touches five agents that already share one
  workspace and one file-naming convention, so the schema is mostly a
  transcription of what the prose already says.

### Phase 2 — Generic channel adapter

Factor `whatsapp_autoresponder.py`'s poll → resolve-identity → call
`/api/chat` → send-reply loop into a reusable module (state-watermarking,
upload handling, expiry sweep included), with three channel-specific
functions left as the extension point: `fetch_new_messages`,
`resolve_identity`, `send_reply`. Reimplement the WhatsApp poller as the
first consumer, to prove the extraction against a real channel before a
second one is attempted. Document the two integration shapes (MCP pull vs.
poller-push) side by side in `scripts/README.md` so a new channel is a
deliberate choice between two named patterns, not a fresh design.

### Phase 3 — Point self-service at the general path

Either extend `tool-builder` to also scaffold `mcp_servers:` entries (the
already-hot-reloadable path) as the default recommendation, or add a
sibling `mcp-builder` agent, and have `tool-builder` explicitly say when a
raw C++ tool (its current scope) is the right call instead — chiefly, when
`fs_guard`/`net_guard`-level trust is required and an external MCP process
is not appropriate.

### Phase 4 — Delete single-project agents in favor of config

- `book-editor`: move the CMOS ruleset out of the system prompt into
  `book/style.md`, read via `read_file`/`recall` the same way the prompt
  already says "the author's remembered rules win" — push that until the
  agent itself carries zero book-specific content, only editing method.
  This turns it into a generic "manuscript editor" reusable for a second
  book with a different style file.
- `astro-ph-summarizer`: fold into `rss-reader` as a configured feed
  (verify tool-list parity first) rather than keep a near-duplicate agent
  alive.

### Phase 5 — Archetype defaults instead of per-incident tuning

`funes.yaml` and `researcher.yaml` carry dated comments explaining a
specific `tool_limits` value chosen after a specific runaway run. That
reasoning is sound but not propagated — a new agent's author has to
rediscover "20 searches, no synthesis" independently. Define 3–4 archetype
defaults (orchestrator, researcher, pipeline-worker, stateless-sub-agent)
with pre-tuned `max_steps`/`tool_limits`, documented once in
`agents/README.md`, so `agent-builder`'s interview can pick one instead of
guessing a number.

## Sequencing

Phase 1 first: it's the most direct repeat of an already-proven fix, touches
no external services, and is entirely within the existing `pipelines`-style
config pattern. Phase 4 is the cheapest (prompt edits only) and can run in
parallel. Phases 2 and 3 are infrastructure work with no urgency — do them
when the next channel or tool request actually arrives, rather than
speculatively.
