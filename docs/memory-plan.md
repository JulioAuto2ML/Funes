# Memory architecture — diagnosis and plan

Status: phases 1–3 **done** in 7.1.0, 4–5 in 7.2.0 (2026-11-10); 6–7 proposed. Numbers
in the diagnosis are from yoda's `data/memory.db`, read-only, on 2026-11-10.

## Progress

`tests/memory_eval` (semantic, k=4, 30 queries over a synthetic pool shaped
like yoda's):

| after | recall@4 | hit@1 | stale-first | contamination@4 |
|---|---|---|---|---|
| baseline, 7.0.1 | 0.750 | 0.700 | 0.167 | 0.142 |
| phase 2, bounded writes | 0.817 | 0.700 | 0.167 | 0.142 |
| phase 3, provenance + taint | 0.833 | 0.700 | 0.100 | 0.108 |

Phases 4–5 needed a model, so they were measured against yoda (Qwen 27B
IQ4_XS on :8080, nomic on :8081). yoda's vectors score the same 7.1 pool
slightly higher than the laptop's CPU run, so the comparison row is re-run:

| yoda, k=4 | recall@4 | hit@1 | stale-first | contamination@4 |
|---|---|---|---|---|
| 7.1, exchanges logged | 0.858 | 0.700 | 0.100 | 0.100 |
| 7.2, extracted, run 1 | 0.767 | 0.700 | 0.067 | 0.108 |
| 7.2, extracted, run 2 (with retries) | 0.742 | 0.700 | 0.067 | 0.075 |

Read honestly: extraction makes the pool cleaner (stale-first and
contamination down, 22 memories instead of 38) and costs recall@4. The
recall lost is answers that lived only inside an exchange — the assistant's
opinion of a car, its yerba suggestion, the amount it read off an attached
bill. The 27B declines to store those because the prompt forbids storing the
assistant's answers (one of them, "the Spanish edition goes out every
weekday", was wrong). The transcript still holds them. Two runs also
differed — the CV's facts were extracted once and not the second time — so a
single run is not a measurement of a prompt change.

Qwen3-8B (CPU, laptop) was worse than logging on every metric: it extracted
the assistant's recap of its own memory as new facts and used `replaces` to
retire richer memories. Three changes came out of that run and stay:
restatements (≥0.85 cosine to a live memory) are dropped unless the model
claims they replace something; `replaces` may only retire `extracted` or
`auto` memories; `extracted` weighs 1.15, not 1.3.

"What's my name?" still fails, now for a different reason: the stale
negative is gone (no fact is extracted from it), but the English question
does not reach the Spanish "El usuario se llama Julio." That is a
cross-language recall problem, not a memory-model one.

What changed from the proposal while building it:

- **Attribution is applied when a memory is read, not stored.** Storing
  "per url:host: …" moved the vector enough to bury a web fact for the very
  question it answers (GPT-6 query: found → not in the top 4). The claim is
  stored bare with `origin`/`tainted` beside it, and `Memory::presented()`
  adds the attribution in the agent's memory block, the `recall` tool and
  `GET /api/memories`.
- **Taint is deny-by-default.** Every tool taints except a short list of
  built-ins that return the caller's own data (`memory_provenance.cpp`,
  `kOwnData`) — MCP tools, generated HTTP tools, `run_script`,
  `delegate_to_agent` and `read_result` included. A run's auto-memory is
  marked tainted too (not re-attributed; `I replied:` already says whose
  words they are), so it anchors no link expansion.
- **What 1–3 cannot fix:** "what's my name?" still ranks the stored "I don't
  have your name" exchange first. That is defect 1 and 3 — exchanges stored
  as memories, nothing superseded — which is phases 4 and 5.

## The question

The 5.0.1 review left one substantive question open: is a memory that is a
conversation log with vector search the product's claim or its weakest part?
The production pool answers it.

## What the pool actually holds

| | count | avg chars | max chars |
|---|---|---|---|
| `auto` — `User said: "…" — I replied: "…"` | **209** (75%) | 499 | 14,492 |
| `consolidated` — merged near-duplicates | 36 | 382 | 1,130 |
| `tool` — the model called `remember` | 31 | 242 | 715 |
| `user` — taught deliberately | 2 | 78 | 87 |

Links: **0**. Link judgments: **0** — the backfill was off until 7.0.

The most-recalled `auto` memories, by `recall_count`:

| recalled | what it is |
|---|---|
| 20 | "what do you remember?" — and the reply listing it |
| 17 | an attached WhatsApp image |
| 15 | "summarise what we've talked about" |
| 15 | an attached Edenor bill (PDF) |
| 15 | "what's my name?" — **"I don't have your name recorded"** |

So the five memories Funes reaches for most are: two conversations *about*
memory, two attachments, and a stale negative that contradicts a fact it does
hold (`user`: "El usuario se llama Julio."). Every one of those recalls put the
stale record into a prompt beside the true one.

## Diagnosis

Six defects, each visible in the numbers above.

1. **Exchanges are stored as memories.** `agent.cpp` writes every person-
   authored turn as `User said … I replied …`. That duplicates `turns` (which
   already keeps the transcript) and turns recall into "find a past
   conversation that looks like this one", which is why questions about memory
   recall other questions about memory. A fact the person stated once is in
   there, but wrapped in the whole exchange and outweighed by everything around it.
2. **Nothing bounds a memory.** The user half is stored whole, so an attached
   PDF becomes a 14.5k-char memory: too long to embed (the 7.0.1 incident), and
   a quarter of a small model's context if it is ever recalled.
3. **Nothing is ever superseded.** "the TV job runs as a script" and "the TV
   job was replaced by two script jobs" are both live `tool` memories. "I don't
   have your name" coexists with the name. Recall returns whichever is closer
   to the query wording, not whichever is true now.
4. **"Recalled" is read as "useful".** Pruning drops `auto` memories that were
   never recalled in 30 days. 175 of 209 have been recalled at least once,
   mostly by recall of their own kind, so the clutter protects itself.
5. **Tool-sourced text is stored verbatim and trusted most.** `remember` from a
   run that read a web page or an email stores what the model chose to copy,
   at `source=tool` weight 1.3, the same as something the person taught. This
   is 5.0.1 item #8, still open; the prompt framing is a mitigation.
6. **There is no measurement.** The 1.3 weight was tuned by hand against one
   query. No change to recall can currently be shown to help.

## Principles

Taken from what already worked in this repo, not new ones:

- **Store facts, keep conversations.** `turns` is the conversation log. A
  memory is a claim about the world, one per row, short enough to embed and to
  read in a prompt.
- **Deterministic first, model only for judgment** (the standing rule).
  Bounding, provenance, supersession bookkeeping and eviction are code. Only
  "what fact, if any, did this exchange establish?" and "does B replace A?" need
  a model, and both are already the shape of `judge_link`: a background,
  schema-checked, recorded, gated call.
- **Additive and reversible.** Nothing is deleted by a migration; superseded
  and archived rows stay in the table with a reason, excluded from recall.
- **Measure before tuning.** An evaluation set comes before any weight changes.

## Proposals

### A. A recall evaluation, first

`tests/memory_eval/`: a fixture database built from a redacted copy of yoda's
pool, plus ~40 questions Julio actually asks ("what's my name", "what car am I
looking at", "when does the Spanish issue go out", "what did we decide about
the TV job") with the memory ids a correct answer needs. A tool reports
precision@4 and *contamination@4*: how often a stale or contradicting memory
is in the top four. Runs offline against the CPU embed server. Every later
phase reports its before/after on this.

### B. Bound what is written (no model)

- Attachments are stored as their reference (`[attached: Edenor_202604.pdf]`),
  never their extracted text — the file is already in the workspace.
- The user half is capped like the reply half already is (300 chars), cut on a
  sentence boundary.
- `remember` refuses text over 500 chars with an error asking for one
  sentence; the tool description already says "one self-contained sentence".

Fixes defect 2 immediately and shrinks defect 1's damage without changing what
is remembered.

### C. Provenance and taint (no model)

New columns: `origin` (`turn:<id>`, `tool:<name>`, `url:<host>`) and `tainted`
(the run had called a tool that returns third-party content — `web_fetch`,
`web_search`, any MCP tool, mail — before this `remember`). The run already
knows this; it is a flag on `ToolContext`.

- A tainted memory is stored as attribution ("per <host>: …"), weighted 1.0,
  not 1.3, and is never the anchor of a link expansion.
- `source=user` stays reserved for the UI's add-memory route and the person's
  own words.

Closes 5.0.1 #8 by design rather than by prompt.

### D. Facts instead of exchanges (model, background)

`FUNES_AUTO_MEMORY=extract` (new default; `log` keeps today's behaviour, `0` stays off):

1. After a `Persist::Full` run, the turn id is queued — nothing on the chat path.
2. A background worker, gated by `busy_gate` like the link backfill, gives the
   model the exchange and the user's top recalled memories, and asks for
   `{"facts": [{"text", "kind", "replaces"}]}`, checked with `answer_schema`.
   `kind` is `about_person | preference | plan | decision | event`; an empty
   list is the common, correct answer. Questions, greetings, and anything that
   is about Funes's own memory produce none.
3. Each fact is stored as `source=extracted`, `origin=turn:<id>`, bounded by B.

The "what's my name? — I don't know" exchange produces no fact, so it stops
existing as a memory. "Me llamo Julio" produces one.

### E. Supersession (model, reuses the link judge)

The link backfill already asks for a `contradicts` verdict. Make it act:
when two memories of one account contradict, the newer one supersedes the
older (`superseded_by`, `superseded_at`); recall excludes superseded rows.
Extraction's `replaces` field does the same at write time when the model
names an existing memory. `user` memories are only superseded by `user`
memories. Every supersession is shown in the UI's memory panel with an undo.

### F. Migrate the existing pool

A resumable background pass over the 209 `auto` rows: run D's extraction on
each, store the facts, set the `auto` row `archived=1`. Archived rows stay out
of recall but remain in the table and the UI. On yoda this is ~209 model calls,
gated — one or two nights.

### G. Eviction that measures use, not recall

Replace "never recalled" with "never *cited*": count a recall only when the
memory's terms appear in the reply (cheap and deterministic), and prune
`extracted` facts that are neither cited nor linked after 90 days. `user`
memories are never pruned, as now.

## Order of work

| Phase | Deliverable | Model calls | Size |
|---|---|---|---|
| 1 | A — evaluation set + baseline numbers | none | ~3 days |
| 2 | B — bounded writes | none | 1 day |
| 3 | C — provenance + taint | none | ~1 week |
| 4 | D — extraction replaces auto-memory | background | ~1 week |
| 5 | E — supersession from `contradicts` | background | ~3 days |
| 6 | F — migrate the 209 | background, one-off | ~2 days + 2 nights |
| 7 | G — cited-not-recalled eviction | none | ~2 days |

Phases 1–3 need no model and could start now. Phase 4 is the change in kind;
5 and 6 depend on it. Until 6 runs, the link backfill (on since 7.0) will
spend most of its calls relating conversation logs to each other — worth
limiting it to non-`auto` memories until then (one `WHERE` clause).

## Tests

House rule: every claim gets a test that fails without the code.

- B: a 14k-char attachment exchange stores under the cap and names the file.
- C: `remember` after `web_fetch` in the same run is tainted, weighted 1.0,
  attributed; without a fetch it is not. `test_user_isolation` gains a case
  for the new columns.
- D: a scripted mock LLM returns `{"facts": []}` for "what's my name?" and one
  fact for "me llamo Julio"; nothing is written on the chat path.
- E: two contradicting memories → only the newer is recalled; undo restores.
- The eval (A) is not a pass/fail test; it is a report checked into the PR.

## Open questions

1. **Do memories stay per agent?** 243 of 278 are `funes`'s; `voc-researcher`
   and `researcher` hold research notes that read like a cache. Should research
   notes leave the memory store for the workspace?
2. **Is the 300-char reply half worth keeping at all** once facts are
   extracted? Proposal D drops it; the transcript is in `turns`.
3. **The person's own corrections.** "No, that's wrong" in chat could feed
   supersession directly. In scope for D, or a later phase?
