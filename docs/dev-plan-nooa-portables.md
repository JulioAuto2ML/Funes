# Dev plan: porting NOOA's transferable ideas into Funes

> **Status: all four shipped in 2.0.** Three deviations from the plan below,
> each noted where it happens: the schema validator is hand-rolled (option B)
> rather than vendored, `consolidate()` takes a merge callback rather than an
> `LLMClient` so it can be tested without a network, and feature 3 needed no
> code in `delegation.cpp` at all — a specialist's answer is an ordinary
> `ToolResult` and crosses the same threshold as any other tool output.

Follow-up to `docs/nooa-comparison.md`. Three features, ordered so each ships
independently and the riskiest interactions come last. Decision already made:
**result persistence is session-scoped** — results live and die with the chat
session, so we do not grow a second long-term store next to `MemoryStore`.

Priorities:

1. Result store + bounded previews (biggest token win, enables 3's delegation fix)
2. Typed final answers (extends the existing completion contract)
3. Delegation by reference (small once 1 exists)
4. Memory consolidation (independent, can ship any time)

---

## 1. Session-scoped result store + bounded previews

**Problem.** Every `ToolResult.text` goes verbatim into the transcript as a
tool message (`agent.cpp` run_loop, ~line 445). A 40KB `web_fetch` costs ~10K
tokens on every subsequent turn until `compress_context` lossily destroys it.

**Design.** Pass-by-reference at the tool-call layer, no REPL needed. Large
results are stored once, addressed by id, and the transcript carries a bounded
preview shaped like NOOA's: type/size + head/tail sample + the id to fetch more.

### Storage

New table in the existing SQLite db (`memory.h` migrate()):

```sql
CREATE TABLE tool_results (
  id         INTEGER PRIMARY KEY,
  session    TEXT NOT NULL,
  agent      TEXT NOT NULL,
  tool       TEXT NOT NULL,
  text       TEXT NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX idx_tool_results_session ON tool_results(session);
```

`MemoryStore` methods: `store_result(session, agent, tool, text) -> id`,
`get_result(session, id) -> optional<text>`, `prune_results(session)` (called
on session delete; optionally also drop rows older than N days at startup).
Session-scoped means: `get_result` filters by session — an agent can never
dereference another session's results. Delegated sub-agents share the caller's
session (delegation.cpp already passes ctx through), so handles cross the
delegation boundary for free.

### Threshold and preview

In `run_loop`, after `dispatch_tool`:

- `result.text.size() <= kInlineLimit` (start: 2048 bytes) → current behavior,
  unchanged. Most tool results (recall, list_tools, shell status) stay inline.
- Larger → store, and the tool message becomes:

```json
{"result_id": 17, "tool": "web_fetch", "bytes": 41320,
 "head": "<first ~1000 bytes, UTF-8-safe>",
 "tail": "<last ~200 bytes>",
 "note": "Full result stored. Use read_result(id=17, offset, limit) to read more."}
```

Reuse `truncate_utf8_safe` from text_utils for both slices. The preview states
the concrete size — NOOA found models handle previews well when the shape is
explicit ("the model reads that shape and understands the name refers to a
real object").

### New native tool

`read_result(id, offset=0, limit=4096)` — returns a window of the stored text.
Windowed, not whole-value, or one dereference re-imports the 40KB we evicted.
Register in a new `register_result_tools(reg, store)`; add to agent allowlists
that have large-output tools (web_fetch, read_file, execute_shell, pdf
extraction).

Note the self-reference: `read_result`'s own output is capped by `limit` ≤
kInlineLimit, so it never triggers storage itself.

### Interactions to handle

- **compress_context / ContextCompressor**: previews compress fine (they're
  small). But after pruning, `result_id`s referenced in the summary may still
  be dereferenced — fine, the store outlives the transcript within a session.
- **`last_tool_result` fallback paths** (loop detector, empty-answer fallback,
  max_steps bailout all echo `last_tool_result`): echo the preview, not the
  full text, for stored results.
- **`require_tools`/satisfied**: unaffected — storage happens after success is
  recorded.
- **Images** (`ToolResult.images`): out of scope; they already bypass text.

### Tests

`test_result_store.cpp`: store/get/session isolation/prune; threshold boundary
(2047/2048/2049 bytes); preview UTF-8 safety on multibyte input; `read_result`
windowing and out-of-range offsets; mock_llm.py integration turn that fetches
a large result and dereferences it.

**Estimate:** ~250 lines core + tests. No YAML schema change.

---

## 2. Typed final answers

**Problem.** `require_tools` validates that side effects happened, not that
the answer is well-formed. Agents whose output feeds another agent (or the
newsletter pipeline) can return malformed prose that parses as success.

**Design.** Extend the completion contract with an optional output schema,
validated in the same loop that already nudges on missing tools. This is
NOOA's "validated termination" without new machinery: reuse the nudge budget,
`force_tool_call` stays false for schema nudges (we want text, just correct
text).

### YAML

```yaml
# agents/researcher.yaml (example)
answer_schema:
  type: object
  required: [summary, sources]
  properties:
    summary: { type: string }
    sources: { type: array, items: { type: string } }
```

`AgentConfig` gains `json answer_schema;` (empty = no contract, today's
behavior). Parse in agent_config.cpp; `from_string` tests.

### Validation

nlohmann/json has no schema validator built in. Two options:

- **A (recommended):** vendor `pboettch/json-schema-validator` (~2 files,
  MIT, builds on nlohmann) into third-party/.
- **B:** hand-roll the subset we need (type/required/properties/items/enum).
  ~150 lines, no dependency, but it will grow.

Start with A unless third-party policy says otherwise.

> **Shipped: B** (`src/core/answer_schema.cpp`, ~160 lines). A is five source
> files and several thousand lines to enforce five keywords, and its errors are
> written for a spec-literate human. The nudge quality is the feature here — a
> 7B model needs `sources[1]: expected string, got number`, not a JSON-pointer
> and a keyword name — and that argues for owning the error strings.
> Unrecognised keywords are ignored rather than rejected, so a schema written
> against the full spec degrades to the subset instead of failing shut.

### Loop changes (`run_loop`)

Where a plain-text answer is currently accepted (the `resp.tool_calls.empty()`
branch, after the tool contract check):

1. If `answer_schema` empty → return as today.
2. Else extract JSON from the answer (raw parse, then fenced ```json block —
   local models fence habitually).
3. Valid → return the canonical JSON (dump of the parsed object, not the raw
   text) so downstream consumers get clean input.
4. Invalid → nudge with the specific validation error, same
   `nudges_used`/budget as the tool contract. Budget spent → explicit
   `contract_failure`-style error, never a silently malformed answer.

The bailout paths (loop detector, max_steps) must also fail loudly when a
schema is set — a "Done. web_search completed:" synthetic answer can't satisfy
a schema and shouldn't pretend to.

System prompt: when a schema is set, append an auto-generated "Your final
answer must be a JSON object matching: …" block at config load, so prompt and
contract can't drift apart.

### Tests

`test_completion_contract.cpp` extension: valid/invalid/fenced answers, nudge
then correct, budget exhaustion, schema+require_tools combined (tool nudges
take precedence — side effects before formatting).

**Estimate:** ~200 lines + vendored validator. Independent of feature 1.

---

## 3. Delegation by reference

**Problem.** `delegate_to_agent` returns one string; big specialist findings
either get re-serialized into the answer or lost.

**Design.** Two small changes once feature 1 exists:

- The specialist's answer passes through the same threshold logic: >
  kInlineLimit → stored, orchestrator gets preview + `result_id`. Because
  the sub-agent already runs in the caller's session, `read_result` works
  across the boundary with zero extra plumbing.
- Specialist prompts (researcher.yaml) get one line: "If you produced a large
  artifact, mention its result_id so the caller can read it."

If a specialist's answer_schema includes a `result_ids: array` field, feature
2 makes referencing structured — but that's an agent-authoring choice, not
mechanism.

**Estimate:** ~30 lines in delegation.cpp + prompt edits. Depends on 1.

> **Shipped as 0 lines of mechanism.** A specialist's answer is returned as an
> ordinary `ToolResult`, so run_loop's threshold already stores it and hands
> the orchestrator a preview; the sub-agent already runs in the caller's
> session, so `read_result` works across the boundary. What shipped is the
> prompt half: the line in researcher.yaml, a sentence in the
> `delegate_to_agent` description, and `read_result` on the allowlists.

---

## 4. Memory consolidation

**Problem.** `remember` auto-stores every exchange, dedup is exact
(agent, text) only, nothing is ever merged or forgotten. The store grows
monotonically and recall precision decays as near-duplicates accumulate.

**Design.** A periodic offline reflection pass, NOOA-style but minimal. No
importance scores, no activation graph in v1 — just dedup and prune, the two
operations with measurable payoff and no new model-facing surface.

### Mechanism

`MemoryStore::consolidate(agent, llm)` — runs in a background thread (same
pattern as `backfill_embeddings`), triggered at server idle or every N hours:

> **Shipped as** `consolidate(MergeFn, ConsolidationOptions)`: the LLM arrives
> as a `std::function<std::string(const std::vector<std::string>&)>` rather
> than an `LLMClient*`. Same wiring in main.cpp, but memory.cpp no longer
> depends on the LLM transport, and the "mock LLM that fails mid-run" test the
> guardrails call for is a three-line lambda instead of a fake HTTP server.

1. **Near-duplicate merge.** For each memory, find neighbors above cosine
   ~0.92 (sqlite-vec query we already have). Batch each cluster into one LLM
   call: "merge these into a single self-contained sentence, or answer KEEP
   ALL if they are distinct facts." Replace cluster with merged row
   (`source = "consolidated"`), delete originals. Embedding for the merged
   row via the existing embed path.
2. **Prune.** Delete `source = "auto"` memories older than N days that have
   never been recalled. Requires a `recall_count`/`last_recalled_at` column —
   add in migrate(), increment in recall paths. Never prune `source = "user"`
   (explicit `remember` calls are protected, mirroring NOOA's protected
   types).

Keyword-only mode (no embedder): skip step 1, run step 2 only. Log a summary
line per run (merged X, pruned Y) — no UI needed in v1.

### Guardrails

- Consolidation must be crash-safe: do each cluster in one transaction
  (insert merged, delete originals, commit).
- Cap LLM calls per run (e.g. 20 clusters) so a huge backlog doesn't burn a
  local model for an hour; the next run continues.
- A `FUNES_CONSOLIDATE=off` env switch for debugging.

### Tests

`test_memory.cpp` extension: recall_count tracking; prune respects
source/age/recall; cluster transaction atomicity with a mock LLM that
sometimes fails mid-run; keyword-only mode skips merge.

**Estimate:** ~250 lines. Independent of 1–3; touches only memory.cpp and a
small scheduler hook in main.cpp.

---

## Sequencing

```
1. result store ──> 3. delegation by reference
2. typed answers (parallel with 1)
4. consolidation (any time)
```

Ship each behind its own trigger: 1 activates only when a result exceeds
kInlineLimit, 2 only when an agent declares answer_schema, 4 behind the env
switch defaulting on. Existing agents and tests must pass unchanged with all
features merged but no YAML edits — that's the regression bar.

## Deliberately not doing (from the paper)

- CodeAct / model-written code: wrong trade for local models on a personal
  machine (see comparison doc, security + small-model stress results).
- Model-visible object state / context blocks: Funes agents are stateless per
  request by design; memory + session summary cover the need at this scale.
- ACT-R activation ranking and memory graphs: revisit only if consolidation
  proves insufficient — measure recall quality first, add machinery second.
