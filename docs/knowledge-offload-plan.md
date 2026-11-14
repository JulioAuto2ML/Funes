# Knowledge offloading — a research plan on Funes

Status: **plan** (2026-11-14), a research track meant to become a paper
(see "The paper"), starting soon; not a Funes version by itself. Nothing
built. Source: a conversation julio had
about moving a model's factual knowledge into a local database and leaving its
parameters for skills (`docs/LLM memory.txt` on yoda). This turns that idea
into questions Funes can actually answer, experiments that answer them on the
hardware there is, and an order to run them in.

## Which Claude for which part

Prices per million tokens in/out (2026-11): Fable 5.1 $10/$50, Opus 5.5
$4/$20, Sonnet 5.5 $2/$10, Haiku 5.5 $0.10/$0.50. On a subscription the
same ratios roughly decide how fast the limit goes. Effort is a second lever as big as the
model: a stronger model at `medium` often beats a weaker one at `high`, so
raise effort before raising the model.

| part | model · effort | why |
|---|---|---|
| Design decisions: hypotheses, statistics plan, what a surprising result means, the paper's argument | **Fable 5.1** · high, a few sessions only | Where a reasoning mistake costs weeks or a retraction. Short sessions with lots of thought, not long coding runs. |
| Driving the project day to day; Funes C++ (typed fact table, `lookup` route, `memory_eval` modes, E7 skill suite) | **Opus 5.5** · high | The core has isolation, taint and safety layers that compose; a change there needs the whole `CLAUDE.md` in mind. |
| Python harness (E0 runner, scorers, plots), venv setup, LMLM / Co-LMLM integration | **Sonnet 5.5** · medium | Ordinary scripting with clear specs, at half Opus's price. Escalate to Opus when the bug is subtle, not when the file is long. |
| Reading papers, literature sweeps | **Sonnet 5.5** subagents to read, Opus 5.5 to synthesise | Reading fills context; the synthesis is the part worth paying for. |
| Running the experiments, swapping models on yoda, watching runs | **no model** — scripts and cron | A run that needs a model to babysit it is a run that isn't reproducible. Haiku 5.5 at most, to summarise logs. |
| LLM judge (only where exact match can't grade) | **Haiku 5.5** or **Sonnet 5.5**, one fixed version, Batch API | Cheap and frozen for the whole study; its agreement with 100 hand labels goes in the paper. Changing the judge mid-study invalidates every earlier number. |
| Generating E5's training data, if `LMLM-Annotator` on yoda isn't enough | **Haiku 5.5** via Batch API (50% off) | Volume work; check a sample by hand. Local first, though. |
| Writing the paper | **Opus 5.5** with the `julio-writing-style` skill; **Fable 5.1** for the final adversarial review | Drafting is long; the reviewer-style critique is short and is where Fable earns its price. |

Not a rule for the models *under study*: those are the local ones on yoda.
Claude builds and reviews; it never answers a benchmark question.

### When to switch — said out loud, not guessed

Every step in "Order of work" names its Claude and effort. The session
working on this project **says so before starting**, in one line, whenever
what it's about to do doesn't match the model it's running on:

> ⚠ Model: this is E4's C++ (Opus 5.5 · high); this session is Sonnet 5.5.
> Switch, or go ahead?

and waits for the answer — it doesn't switch on its own, and it doesn't
quietly carry on. The triggers:

- **A step boundary** in "Order of work" whose model or effort differs.
- **Escalate** when a bug has survived two honest attempts, when a result
  contradicts a hypothesis, or when the next decision changes the paper's
  claims (a metric, an exclusion, a dataset).
- **Step down** when an expensive session is doing mechanical work —
  plotting, renaming, re-running a script, editing prose for typos.
- **Effort first**: when the model is right but the work got harder or
  easier, propose an effort change before a model change.

### Switching without dragging the conversation along

A switch is a **new session**, never the same conversation continued on
another model: prompt caches are per model, so the new one would re-read
the whole transcript at full price, and most of it is noise by then. What
carries over lives in files:

- **This plan** — what and why; changes only when a decision changes.
- **`benchmarks/knowledge-offload/NOTEBOOK.md`** — the lab notebook. Each
  session ends by appending a dated entry of ≤ 15 lines: step, what was done,
  decisions made (and why), results by pointer (`results/<run>.json`, a plot
  path — never pasted numbers beyond a headline), open problems, the exact
  next action, and the Claude that action needs. The top of the file holds a
  "Now" block (current step, blocker, next action) rewritten every session.
- **`results/`** — the raw JSON; the notebook and the paper cite it, nobody
  pastes it into a conversation.
- **Decisions** that change the paper (an exclusion, a metric, a judge)
  go in the notebook *and* as a dated line here, so the paper's methods
  section can be written from the record, not from memory.

A new session starts by reading the plan's relevant section, the notebook's
"Now" block and its last three entries — a few thousand tokens, not a
conversation. If that isn't enough to continue, the notebook entry was too
thin, and that is the thing to fix.

## What it changes in Funes

Little, and only where a result asks for it. Most of the plan *uses* Funes
— its API with throwaway accounts, `MemoryStore`, the harness logs — and
lives in `benchmarks/knowledge-offload/`, outside the runtime. Changing the
model is config (`launch_llama.sh`, `FUNES_LLM_URL`), independent of all this.

| piece | where | lands in Funes only if |
|---|---|---|
| E0–E3, E1b, E5, E6 | `benchmarks/`, Python, separate venv | never — research code |
| E7's skill suite | `tests/` beside `memory_eval` / `heal_eval`, or `benchmarks/` | it's worth keeping as a regression check on model changes |
| `require_tools: [recall]` for agents that answer factual questions | agent YAML | E3 shows forced lookup beats letting the model decide |
| a typed fact table (subject, predicate, object, provenance) + exact-key lookup beside vector recall | `memory.cpp`, a tool | E4's adaptive store clearly beats today's recall |
| a `lookup(entity, relation)` route so an LMLM-style model can use the store | `api.cpp` or a read-only adapter outside | E1b step 4 works and something will actually call it |
| a smaller default model | config | E7 finds a floor ≤ 8 GB |
| a shared world-knowledge pool (below) | `memory.cpp`, `users.cpp`, import CLI | a smaller default model needs facts it no longer carries |

So a version number belongs to whichever of the last four rows ships, if any,
not to the plan.

### Facts Funes doesn't have

The stores the experiments need are **world** knowledge — PopQA's Wikidata
triples, LMLM's 54.6M-triple DB — and `memory.db` holds a household's
personal facts. Two different situations:

- **During the experiments**: a separate scratch database (`FUNES_DB`
  pointed elsewhere), never yoda's `data/memory.db`, not even under its own
  account. Isolation would hold (per-account pools, partitioned vectors), but
  the background passes wouldn't care: consolidation, link backfill and
  extraction would spend the GPU on millions of rows, and the nightly backups
  would grow with them.
- **If E7 succeeds**, a smaller model needs world facts in production, and
  that is a real design change, not an import:
  - **Whose pool?** Not a person's. A shared, read-only knowledge pool —
    like the `machine` account (7.7), a system owner nobody logs in as —
    recalled for every account, never consolidated, pruned or superseded by
    a conversation; replaced only by re-importing a versioned dataset.
  - **Vectors don't scale to it.** sqlite-vec's vec0 is brute-force KNN, and
    54.6M × 768-dim floats is ~170 GB. A world store is triples with exact
    and FTS lookup (E4's typed table), with vectors kept for the personal
    pool — or an external ANN index, which is a dependency Funes has avoided.
  - **Provenance and taint.** An imported fact carries its dataset and
    version. Is it own data or outside content to the effects gate? It's
    admin-installed like `scriptlib/`, which argues for untainted — but it's
    still third-party text read into a run. Decide before shipping, not after.
  - **Personal over world.** When the person's pool and the world pool
    disagree ("my doctor is X" vs a public record), the personal one wins, and
    recall ranking has to say so (`source_weight`).

### Or don't install it: look it up online

The alternative to a world store is the one Funes already has —
`web_search` (Parallel, Tavily fallback) and page reading. It costs some
local-first, but it may well be the better default. What it trades:

| | world store | web |
|---|---|---|
| install | tens of GB, versioned imports | nothing |
| freshness | as old as the import | today |
| offline | works | doesn't (yoda lost its route out on 2026-11-14) |
| privacy | the question never leaves | every lookup tells a search provider what is being asked |
| latency / cost | ms, free | seconds per lookup, a paid API per call |
| noise | curated triples | pages; a small model has to read them |
| **effects gate** | admin-installed, plausibly untainted | **tainted** — every run that searched proposes instead of acting |

The last row is the Funes-specific one: a small model that searches for
everything turns every later `host`/`external` call in the run into a
proposal (7.11, 7.14). That's the gate working, but it changes what a
"fact lookup" costs.

The middle ground needs nothing new: **demand-driven memory.** What the
model looked up and used is stored — tainted, with its URL, as `remember`
after a page already does — so the store grows only with what this
household actually asks, and the second time is local. Possibly plus a small
curated head (the popular entities, where the model's own weights were
right anyway). The experiments measure all three as conditions (`web`,
`web+cache` beside `store`) rather than picking one now.

## The question

*If facts live in a database the model consults, how much of the model is
left for everything else — and does a small model with the database behave
like a big model without it?*

Split into what can be measured:

1. **Size buys knowledge, not skill?** At fixed task, how much of the gap
   between a small and a large model disappears when both get the same facts
   from outside? If the gap closes on fact-heavy tasks and stays on
   reasoning-heavy ones, parameters were mostly buying recall.
2. **Who wins a conflict?** When the database says one thing and the weights
   another (an edited, superseded or counterfactual fact), which does the
   model follow, and how does that change with size?
3. **When to look up.** Does the model know when it doesn't know — does it
   consult the store before answering, and abstain when the store has nothing?
4. **What shape should the store have?** Funes's vectors + FTS + links,
   versus subject–predicate–object records with exact lookup (Knowledge
   Objects), on near-duplicates that embeddings can't tell apart.
5. **Can behaviour be trained, cheaply?** A LoRA that teaches a small model to
   look up instead of recall, LMLM-style — what it costs closed-book, what it
   gains with the store, and what it does to unrelated skills.
6. **Where does the knowledge sit?** Removing or damaging layers of a small
   model: what breaks first, recall or reasoning, and does external evidence
   repair the recall part.

Questions 1–4 are inference only and run on yoda as it is. 5 needs training
time. 6 needs weights in memory and an interpretability tool, small models only.

## Feasibility, honestly

What Funes cannot do: **actually free parameters.** That needs pre-training
with facts masked out of the loss (LMLM, Co-LMLM) or anonymised entities
(KLLMs), or learned pruning with memory modules (KOFF) — GPU-weeks, not a
16 GB card shared with production. A LoRA changes *behaviour* (look up first),
not capacity. So the claim this plan can support is the empirical one —
*"a small model + an external fact store recovers X% of a large model's
factual accuracy at Y× lower cost, and loses Z on tasks that need no facts"* —
not "we built a knowledgeless model".

What Funes already has that the papers don't:

| papers assume | Funes has, in production |
|---|---|
| a clean, static fact DB (Wikidata dump) | facts extracted from real conversations (`fact_extraction.h`), with provenance and taint |
| edits = overwrite a row | supersession with an audit trail (`MemoryStore::supersede`, reversible) |
| retrieval = one KNN | vectors + FTS5 + link expansion, each only appending |
| the model calls the DB when it wants | harness layers that can *force* a lookup (`require_tools`) or withhold tools |
| one user | per-account pools, partitioned vectors |

That is the angle: not replicating LMLM, but measuring the knowledge/skill
split *in a deployed agent with a messy, evolving, multi-source memory*,
which nobody has numbers for.

### Hardware

- yoda: RX 9060 XT 16 GB (ROCm), ~1 GB free while Qwen 27B and nomic are
  loaded. A size sweep means **swapping the production model out** for a
  window (`~/launch_llama.sh stop …`), as is already done for image
  generation — fine, as long as it avoids the newsletter/noticiero crons and
  every run records the window and restores `qwen27b`.
- yoda has 213 GB of free disk and 15 GB of RAM. The released LMLM models
  (below) are 176M–382M parameters (≤1.5 GB fp32): in a model-swap window
  they go on the GPU. PyTorch already runs on the RX 9060 XT there:
  ComfyUI's venv (`~/ComfyUI-Installs/ComfyUI/ComfyUI/.venv`, Python 3.13)
  has `torch 2.12.0+rocm7.14.0` from AMD's index
  (`repo.amd.com/rocm/whl-multi-arch/`, pinned in that install's
  `requirements-amd.txt`), and `~/torch72-test` has `2.12.1+rocm7.2`; both
  see the GPU. The research venv is a **new** one with ComfyUI's torch pin —
  never pip-installing into ComfyUI's own venv, whose image generation must
  not break. LMLM pins torch 2.4 / transformers 4.49, so those pins are
  lifted and the paper's numbers re-checked after (E1b step 1).
- The RAM limit is about the **stores**, not the models: LMLM's 54.6M-triple
  DB and Co-LMLM's 113 GB FAISS index don't fit in 16 GB of VRAM either, so
  they stay in RAM / memory-mapped from disk whichever device the model runs
  on (and a FAISS GPU build for ROCm is not a stock wheel).
- Inference experiments never run on the laptop's CPU (standing rule): they
  call yoda's llama-server endpoints.
- Training (E5) on yoda is only possible for ≤4B models with QLoRA and the
  card to itself; realistic option is a rented GPU for a few hours. Decide
  when E1–E3 say it's worth it.

## Open-weight models that already offload knowledge

Checked 2026-11-14. These are what make the plan more than "a big model with
RAG": models whose pre-training *masked facts out*, each released next to a
baseline trained on the same data without masking. That pair is the
controlled experiment the idea needs, and nobody here has to pay for the
pre-training.

| what | weights | store | runs on yoda? |
|---|---|---|---|
| **LMLM** (Zhao et al., ICLR 2026) — LLaMA-2 176M / 382M pretrained on annotated Wikipedia, emits `dblookup(entity, relation)` and splices the value | `kilian-group/LMLM-llama2-{176M,382M}` + `Standard-llama2-{176M,382M}` baselines, MIT code | `kilian-group/LMLM-database`: 54.6M (entity, relation, value) triples, 7.96 GB JSON; MiniLM fuzzy match at 0.6; precomputed FAISS cache | yes, GPU (CPU fallback). The DB is **plain triples → editable by hand**, and only a subset is loaded at inference (RAM) |
| **Co-LMLM** (Feldman et al., 2607.07707) — SmolLM2 360M on FineWeb-Edu; at `<FACT>` its hidden state *is* the query | `lil-lab/CoLMLM-360M-FW` + `CoLMLM-Standard-LM-Baseline-360M-FW`, plus the fact-span annotator | Wikipedia index: 236M entries, ~113 GB FAISS (fits on disk; needs `LMLM_FAISS_MMAP=1` for 15 GB RAM). A 1.07 TB FineWeb+Wiki index does not fit | yes: model on GPU, index mmap'd from disk (slow lookups). Keys are hidden states, so **a fact can't be edited by typing it** — only by re-encoding a context with the model |
| LMLM annotator — LLaMA-3.1-8B-Instruct LoRA that writes `dblookup` annotations into text | `kilian-group/LMLM-Annotator` | — | as a GGUF on :8080, yes. Useful to build E5's training data |
| Intern-S2-Mobius (2608.14290) — 35B, shared FFN "memory" + attention "reasoners", from Qwen3.5-35B | `internlm/Intern-S2-Mobius`, Apache-2.0, BF16/FP8, custom code | inside the weights, not a DB | no: too large, custom modeling code, no llama.cpp path. Reading only |
| KLLMs, KARLA, KOFF, Mixture-of-Memories | no weights or code found | | no |

What they're small for matters: 382M is a model that completes a biography,
not one that calls tools or follows instructions. They answer *"does
masking facts out of pre-training change what the parameters do?"*, not
*"can Funes run on one?"*. Both questions are in the plan; they use
different models.

## Experiments

All of them live in `benchmarks/knowledge-offload/` (Python, like
`benchmarks/jevbench`: not in `ctest`, needs a live model). Every result is a
JSON file with model, quant, commit, seed, date and the endpoint, plus a
plot. Each configuration runs **at least three times** — the memory plan
already saw a single extraction run differ from the next.

### E0 — harness (week 1)

One runner: `dataset × model × condition → answers → scored`. Conditions:

- `closed` — question only.
- `oracle` — question + the gold fact(s) in the prompt. Upper bound.
- `store` — question + top-k from a fact store (no tool call; retrieval done
  by the harness). Measures the store, not the model's decision.
- `tool` — the model gets a `lookup` tool and decides. Measures the decision.
- `tool+contract` — same, with a lookup required before answering
  (Funes's `require_tools` idea).
- `funes` — the real thing: the question through `/api/chat` of a
  throwaway account whose pool was loaded with the facts. End-to-end, with
  every safety layer in the way.
- `web` — no store; the model gets `web_search` and page reading. Same
  questions, so the long tail can be compared store vs web directly, with
  latency, API cost and taint per answer recorded.
- `web+cache` — `web`, but what was looked up is remembered; run the set
  twice and measure the second pass (and stale answers when the cache is old).

Scoring: exact/alias match for short answers (PopQA-style), LLM-judge only
where unavoidable and then with the judge fixed and its agreement checked on
50 hand-labelled items. Cost per answer: prompt + completion tokens and
wall time from llama-server's own timings.

Done when: PopQA (1k sample) runs in all eight conditions against Qwen 9B and
the numbers repeat within noise.

### E1 — the size curve (weeks 2–3) · flagship

Datasets: **PopQA** (entity popularity is in the data — the long tail is
where weights fail; Mallen et al. 2023), **EntityQuestions**, and a
reasoning control with no facts needed (**GSM8K** sample, or BBH logical
subtasks). Fact store: the Wikidata triples behind PopQA/EntityQuestions,
loaded into a `MemoryStore` pool and into a plain triple table.

Models: one family, as many sizes as fit in 16 GB (e.g. Qwen3.5 0.8/2/4/9/27B
GGUF, same quant where possible).

Plot: accuracy vs log(params), one line per condition, faceted by popularity
bucket. The thing to look at is the **slope**: if `closed` rises steeply with
size and `oracle`/`store` is nearly flat, size was buying recall. GSM8K should
show the opposite (flat external lines are irrelevant there; size still
helps). The ratio of the two slopes is the paper's headline number.

Kill criterion: if `store` doesn't beat `closed` for the 9B on the long tail,
the retrieval is the problem — go to E4 before anything else.

### E2 — conflicts and edits (week 4)

Take facts the model knows closed-book (correct in E1 `closed`), change them
in the store (CounterFact-style counterfactuals, plus realistic updates: a
new CEO, a new capital of a renamed region), ask again in `store` and `tool`.

Measure per size: **context-faithfulness** (follows the store),
**parametric override** (answers what the weights say), mixed answers. Then
the same through Funes with the old fact superseded vs merely contradicted by
a newer row — does supersession (old row out of recall) beat leaving both in?
That last one is a direct test of a Funes design choice.

Expected (and worth confirming or refuting): larger models override more.
If true, it argues *for* small models + store in exactly the setting the idea
is about.

### E3 — knowing when to look up (week 5)

Mix of three question kinds: answerable from the store, answerable only from
weights (general knowledge not in the store), unanswerable (in neither —
invented entities). Conditions `tool` and `tool+contract`.

Measure: lookup rate per kind, abstention on unanswerable, hallucination
rate, and accuracy when the store is *wrong* (E2's edits) or *noisy*
(distractor facts — the KLLM paper's noisy-retrieval setting). This is the
"metacognition" claim from the conversation, with numbers; it also tells
Funes whether `require_tools: [recall]` is worth adding to agents that answer
factual questions.

### E4 — store shape (week 5, in parallel with E3)

Extend `tests/memory_eval` rather than writing a new thing:

- a fixture of **near-duplicates**: same subject and predicate, different
  objects ("Edenor bill March: $41.200" / "April: $43.900"), different
  subjects same predicate, dates;
- a `--store triples` mode: facts also stored as (subject, predicate, object,
  provenance), queried by exact key when the top-k embeddings are too close to
  each other (the KO paper's density-adaptive switch);
- compare recall@k / hit@1 / stale-first / contamination for vector, vector+FTS
  (today), and adaptive.

If adaptive wins clearly, it's a Funes feature (a typed fact table next to
`memories`), not just a result.

### E5 — teach lookup, not recall (weeks 6–8, needs GPU time)

LMLM without pre-training: build SFT data where the answer's factual spans
come from a `lookup(...)` call and its returned value, and **mask the
returned values out of the loss** (the model learns to ask and to use, not to
memorise). QLoRA on a 2–4B model.

Compare base vs LoRA on E1/E2/E3 conditions and on skill benchmarks the
facts don't touch (GSM8K, a BFCL tool-use subset, instruction following).
Interesting outcomes, in order: closed-book drops (the model stopped
trusting itself), `tool` rises above base `tool`, skills unchanged. If skills
*rise*, that's the "freed capacity" claim — unlikely with a LoRA, and worth a
careful second look if it happens.

The training data does not have to be written by hand:
`kilian-group/LMLM-Annotator` already produces `dblookup` annotations, and
the LMLM repo's `utils_mask.py` is the loss masking. E5 is "LMLM's recipe as
a LoRA on an instruction model", which the papers haven't done.

### E1b — the pretrained pairs (week 3, alongside E1)

LMLM vs Standard (176M, 382M) and Co-LMLM vs its baseline (360M): same data,
same size, only the masking differs. The cleanest test of the idea there is.

1. **Reproduce** the paper's PopQA / T-REx numbers on a 1k sample — if they
   don't reproduce on yoda (and after lifting the torch pin), nothing built on top is worth anything.
2. **Where did the capacity go?** Perplexity on the *non-fact* tokens, and
   the NLU tasks (`eval_nlu_task.sh`), LMLM vs Standard. The paper reports
   "no loss"; the stronger claim — the masked model is *better* at what's
   left — is the one the original idea makes, and is open.
3. **Edits** (E2 on a real LMLM): change triples in its database
   (counterfactuals, updates) and measure how often the generated text
   follows the DB. Compare with Standard + the same fact in the prompt
   (RAG). Expected near-100% for LMLM; how far below RAG falls is the number.
4. **Swap the store for Funes's.** Load PopQA's facts as a Funes pool and
   answer LMLM's `dblookup(entity, relation)` from `MemoryStore` (FTS +
   vectors) instead of its MiniLM index. If accuracy holds, Funes is a
   drop-in backend for a knowledge-offloaded model — a small, concrete,
   demo-able result.
5. Co-LMLM only: editability of a store whose keys are hidden states. How
   many re-encoded contexts does it take to make an edit stick?

### E7 — can Funes run on a smaller model? (the motivating question)

The idea started as an engineering question, not a research one: *can a
smaller, "smarter" model fit in less VRAM if it doesn't have to carry the
facts?* On yoda that is concrete — Qwen 27B IQ4_XS takes 13.7 of 16 GB; a 9B
at Q8 about 9.5; a 4B about 4.5. Every GB freed is context, a second model
loaded beside it (the model-switching idea), or image generation without a
swap.

**First, honestly: the LMLM models can't run Funes as they are.** 176M–382M
base models, not instruction-tuned, no tool calls, no chat template. Funes's
loop needs a model that picks tools, writes valid arguments, delegates,
satisfies `require_tools` and an `answer_schema`. So E7 is not "plug LMLM
into Funes"; it is three steps:

1. **A skill suite from Funes itself.** Tasks taken from what the agents
   actually do (pick the right tool, valid arguments, delegate with a
   self-contained task, meet the completion contract, produce the schema,
   stop instead of looping), with every fact a task needs available from the
   store — so the suite measures skill, not recall. Score from the run
   itself: success, plus how often each safety layer had to fire (prose tool
   calls rescued, loop kills, contract and schema nudges, last-step
   synthesis). The harness already logs all of that; a small model that
   "works" only because the layers keep catching it should show up here.
2. **The skill floor.** Run the suite across sizes (same family, E1's
   models) with the store on. Plot success and layer firings vs VRAM (weights
   + KV cache at Funes's real context). The smallest model within a few
   points of the 27B is the answer to the original question for *today's*
   models — no training involved.
3. **Push the floor down** — only if 2 leaves a gap worth closing: E5's
   lookup LoRA applied to the model just below the floor. If the LoRA moves
   it over the line, that is the claim "offloading knowledge buys a size
   class" on a real agent, which is the result the whole plan is aiming at.

Success criterion: a model that fits in ≤ 8 GB of VRAM and is within 5
points of the 27B on the skill suite, with the store. Failure is also an
answer: if skill drops with size even when facts are supplied, then at these
sizes parameters were buying skill, not facts, and the idea needs
pre-training (Co-LMLM scale), not Funes.

### E6 — where the knowledge sits (optional, weeks 8–9)

Small model (≤2B, transformers on yoda's GPU in a swap window, CPU if ROCm torch fights back; no serving, short
runs). Ablate or zero-out MLP blocks layer by layer and measure the drop on
PopQA `closed`, PopQA `oracle`, and GSM8K. If the literature is right
(recall in mid-layer MLPs, reasoning higher up), there is a band of layers
whose removal kills `closed` but not `oracle`: that band is the knowledge
that the store can replace. A "phase diagram" plot — layer × task × condition.
This is the experiment closest to the original intuition ("which weights do
what") and the most physics-like.

## Order of work

| weeks | what | output | Claude |
|---|---|---|---|
| 1 | E0 harness, PopQA sample, one model | runner + first table | Sonnet 5.5 · medium |
| 2–3 | E1 size sweep on yoda in model-swap windows | size-curve plot — the headline | none (scripts); Opus 5.5 to read the result |
| 3 | E1b: venv (ComfyUI's ROCm torch) + LMLM pairs on yoda's GPU, reproduce, then edits and Funes as backend | LMLM vs Standard table | Sonnet 5.5 · medium |
| 4 | E2 conflicts, incl. supersede vs contradict in Funes | faithfulness vs size | Sonnet 5.5; Opus 5.5 for the supersede-vs-contradict design |
| 5 | E3 + E4 | lookup/abstention table; store-shape table | Sonnet 5.5 (E3); Opus 5.5 · high (E4, C++) |
| — | **checkpoint**: write up E1–E4 + E1b, tested against H1–H6 | preprint draft (arXiv) | Fable 5.1 · high to judge results vs hypotheses; Opus 5.5 to draft |
| 6 | E7 steps 1–2: Funes skill suite, size sweep with the store | skill floor vs VRAM | Opus 5.5 · high |
| 7–9 | E5 (rented GPU) on the model below the floor = E7 step 3 | LoRA vs base, on Funes | Sonnet 5.5 · medium |
| 9–10 | E6 if time | layer ablation plot | Sonnet 5.5; Fable 5.1 to interpret |

The checkpoint is deliberate: E1–E4 are a complete result by themselves
(inference only, reproducible by anyone with a llama-server), and should go
out before the expensive part starts.

## Rules

- **One variable per comparison.** Same quant, same prompt template, same k,
  same seed set across sizes; a change to any of them is a new row.
- **Funes's production pool is never the dataset.** Experiments use throwaway
  accounts on a copy of the DB or a scratch DB; nothing personal leaves yoda
  in a result file.
- **Every swap of yoda's model is logged** and undone; not while the
  noticiero or the newsletter crons run (`scripts/yoda.crontab`).
- Negative results are kept and written down — the memory plan's
  "extraction costs recall@4" was the most useful line in it.

## The paper

The experiments are designed to be published, not just to steer Funes.
That changes how they're run more than what they are.

### Contribution, in one sentence

*We measure the knowledge/skill split inside a deployed agent: the same
models and tasks with facts in the weights, in a local store, and on the
web — and what each costs in accuracy, VRAM, latency, privacy and safety
gating.*

What's new against the related work: LMLM, Co-LMLM and KLLMs study isolated
models on QA and perplexity; RAG studies compare retrievers, not where the
knowledge should live; agent-memory papers compare memory designs, not
model size. Nobody has the three-way comparison inside a working agent, and
nobody counts the safety cost of outside content (taint → approvals) as part
of the price of a lookup.

Working title: *Facts Outside the Weights: How Much Model Does a Local
Agent Need?*

### Hypotheses, stated before running

Written here first and committed — the commit date is the pre-registration
(optionally mirrored to OSF). A result is reported against these whether it
confirms them or not; anything found later is labelled exploratory.

| | hypothesis | test | experiment |
|---|---|---|---|
| H1 | Supplying facts flattens the accuracy-vs-size curve on long-tail QA: the `store` slope is < ½ the `closed` slope | slope ratio, bootstrap CI over questions | E1 |
| H2 | It does not flatten it on fact-free reasoning (GSM8K): `store` slope ≈ `closed` slope | same | E1 |
| H3 | Larger models override edited facts more often (parametric override rises with size) | logistic regression of override on log(params) | E2 |
| H4 | Masking facts in pre-training improves the non-fact part: LMLM < Standard perplexity on non-fact tokens at equal size | paired difference per document, CI | E1b |
| H5 | Forcing a lookup (`tool+contract`) lowers hallucination on unanswerable questions vs letting the model decide, at no accuracy cost on answerable ones | McNemar, paired | E3 |
| H6 | `web+cache` reaches `store` accuracy on the second pass, with taint on every cached answer | paired accuracy; taint rate | E0/E1 |
| H7 | A model ≤ 8 GB VRAM with the store is within 5 points of the 27B on the Funes skill suite | difference, CI | E7 |

### Statistics

- **Paired by design**: every condition sees the same questions, so
  comparisons are paired (McNemar for accuracy, paired bootstrap for
  differences), not two independent means.
- **Uncertainty everywhere**: 95% bootstrap CIs (10k resamples over
  questions) on every number in a table or plot. 1k questions gives about
  ±3 points at 50% accuracy — the sample size per cell is chosen from that,
  not from what finishes overnight.
- **Run-to-run noise**: greedy decoding (temperature 0) for the main
  results, and **3 repeats** per cell anyway — llama.cpp on GPU is not
  bit-reproducible across batch sizes, and the memory plan already saw two
  runs disagree. Report the spread.
- **Multiple comparisons**: Holm correction across H1–H7.
- **Confounds named, not hidden**: quantisation differing across sizes
  (measure one size at two quants), prompt template (fixed per family), the
  judge for any LLM-graded item (agreement with 100 hand labels reported).

### Reproducibility

Released with the paper: the harness, every config, GGUF file hashes,
dataset versions and splits, the raw per-question JSON for every run, and
the plotting code — enough that a figure can be rebuilt from the JSON
without a GPU. The skill suite is released as tasks, not as Funes
conversations: nothing from yoda's pool, ever.

### Structure

1. Introduction — the VRAM question, the three places a fact can live.
2. Related work — knowledge-offloaded LMs (LMLM, Co-LMLM, KLLMs, KOFF,
   Mobius), RAG and adaptive retrieval (PopQA, Self-RAG), knowledge conflicts
   (CounterFact, MemoReason), agent memory, layer localisation.
3. Setup — Funes as the testbed (one paragraph and a diagram; the system
   isn't the contribution), models, datasets, the eight conditions.
4. Results — E1 (headline figure), E1b, E2, E3, E7.
5. Analysis — E4 store shape, E6 layers if done, cost table (VRAM, latency,
   API cost, taint rate per condition).
6. Limitations — one GPU, one model family per sweep, small-model LMLMs,
   a skill suite drawn from one installation.

### Where and when

- **arXiv first**, as soon as E1–E3 + E1b hold up (the report checkpoint in
  "Order of work" becomes the preprint draft).
- Then a venue: a NeurIPS or ICLR workshop on memory / agents / efficient
  models is realistic with inference-only results; COLM if E7 lands clearly.
  A main-track ML conference most likely wants E5 (training) as well.
  Check deadlines when the draft exists, not now.
- Author: julio, independent. Acknowledge the LMLM / Co-LMLM authors for
  released weights; worth writing to them once E1b reproduces — they may
  want to know their models run as a backend for a real agent.

## Reading list (verified on alphaXiv, 2026-11-14)

Core:
- Zhao et al., *Pre-training Limited Memory Language Models with Internal and
  External Knowledge* (LMLM) — arXiv:2505.15962
- *Co-LMLM: Continuous-Query Limited Memory Language Models* — arXiv:2607.07707
- *Auditing Forgetting in Limited Memory Language Models* — arXiv:2607.00605
- *Knowledgeless Language Models* — arXiv:2607.12831
- Galliamov, Choenni & Titov, *Knowledge Offloading* (KOFF) — arXiv:2605.29075
- *Facts as First Class Objects: Knowledge Objects for Persistent LLM Memory* —
  arXiv:2603.17781
- Jin et al., *Disentangling Memory and Reasoning Ability in LLMs* —
  arXiv:2411.13504

Close and newer than the conversation:
- *Mixture-of-Memories: modular knowledge injection* — arXiv:2610.25425
- *KARLA: Knowledge-base Augmented Retrieval for LMs* — arXiv:2606.26807
- *Intern-S2-Mobius: decoupled knowledge and reasoning* — arXiv:2608.14290
- *MemLLM: finetuning LLMs to use an explicit read-write memory* — arXiv:2404.11672
- *Demystifying the Roles of LLM Layers in Retrieval, Knowledge, and
  Reasoning* — arXiv:2510.02091 (for E6)
- *Disentangling Recall and Reasoning … Layer-wise Analysis* — arXiv:2510.03366 (E6)
- *The Reasoning-Memorization Interplay … a Single Direction* — arXiv:2503.23084 (E6)
- *MemoReason: parametric memory vs contextual reasoning* — arXiv:2609.35312 (E2)
- Mallen et al., *When Not to Trust Language Models* (PopQA) — arXiv:2212.10511
- Meng et al., ROME / MEMIT (CounterFact) — arXiv:2202.05262, 2210.07229

The conversation also cites Yang, Gao & Wu, arXiv:2507.18178 (dual-system
decoupling); not verified yet.

## Open questions

- Which model family has enough GGUF sizes on ROCm to make E1's curve 5
  points long, and does the 27B have to be IQ4 while the rest are Q8 (a
  confound to either remove or measure)?
- Is the end-to-end `funes` condition worth its noise, or does it belong only
  in a final "and in the real system" row?
- E5's budget: rented GPU hours, or E1b's results say a LoRA isn't needed.
- LMLM pins torch 2.4 / transformers 4.49; Co-LMLM uses uv on Python 3.12.
  Two venvs, not one.
- How much of LMLM's 54.6M-triple DB fits in 15 GB of RAM, and is the
  subset loaded at inference the one PopQA needs?
