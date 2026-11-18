# Facts Outside the Weights: How Much Model Does a Local Agent Need?

**DRAFT 0 · 2026-11-17 · written before the remaining experiments, on julio's instruction (write first, fill in after).**
Author: Julio Rodriguez Martino (independent). Drafted by the Opus session of the knowledge-offload track.
Plan: `docs/knowledge-offload-plan.md` ("The paper"). Lab notebook: `benchmarks/knowledge-offload/NOTEBOOK.md`.

**Golden rule of the lab, applied to this text:** nothing is assumed, everything is measured. Every number below is copied from a committed table and cited with its file and sha256. What depends on a result that does not exist yet is a visible placeholder `[TBD: what, which cell/table, which file]`. No placeholder was filled with an estimate. A claim without a source is marked `[UNVERIFIED]`.

---

## 0. Status of this draft (remove before submission)

### 0.1 Sections

| § | section | status |
|---|---|---|
| Abstract | | **drafted with placeholders** (E1 numbers in; variants, E2/E3, H2, H7 open) |
| 1 | Introduction | drafted |
| 2 | Related work | drafted from abstract-level reading only; **full read of each paper pending** (see 0.4) |
| 3 | Setup: models, data, conditions | drafted in full |
| 4 | Methods: metrics, statistics, validity rules | drafted in full |
| 5.1 | E1 headline: facts flatten the size curve (H1) | **drafted with measured numbers** |
| 5.2 | What the store costs: reading capacity and retrieval | drafted with measured numbers |
| 5.3 | When the model looks up on its own (H8, variants) | measured part drafted; **variants branch-written, blocked on the stage A/B queue** |
| 5.4 | Forcing the lookup: the contract (H5) | drafted with measured numbers and the negative result |
| 5.5 | Web as the store | drafted; 27B cell partial; **blocked on a search budget** for more cells |
| 5.6 | Write-back (H6, exploratory) | drafted (negative) |
| 5.7 | Fact-free reasoning (H2) | placeholder, **blocked on GSM8K arm (not run)** |
| 5.8 | Conflicts and edits (H3); knowing when to look up (E3) | placeholder, **blocked on E2/E3 (not run)** |
| 5.9 | Does it fit a smaller model? (H7, E7) | placeholder, **blocked on the Funes skill suite (not built)** |
| 4.7 | How the work was organised (sessions, rules, channels, failures, limits) | **drafted from the branch; one `[TBD]`** (coordinator's model/effort not in any file; D17 not on the branch) |
| 5.10 | Models with knowledge removed (E1b/E5/E6, H4) | `[OPEN: depends on the route and its results]`; **no claim made** |
| 6 | Cost table (VRAM, latency, API cost, taint) | partly measurable now (wall time); VRAM/taint **not recorded** by the harness |
| 7 | Limitations | drafted, written early on purpose |
| 8 | Reproducibility | drafted in full |
| App. A | Pre-written conclusions for the variants, one per reading | drafted (branches; all but one will be deleted) |

### 0.2 Placeholder count
23 real `[TBD]`, 3 `[OPEN]`, 4 `[UNVERIFIED]` at the first commit; recount with grep, see the last line.

### 0.3 What must be measured before submission, and who

| # | measure | blocks | who | cost (measured / not measured) |
|---|---|---|---|---|
| 1 | Variants stages A+B (12 cells) → `VARIANTS_TABLE.md` | §5.3, App. A | coordinator runs in julio's window (queue is running tonight, ends ≤ 05:45); Opus reads | queue upper bound 4.9 h (notebook, Sonnet entry); real time not yet measured |
| 2 | Stage C (27B) / D (2B) only if rule R3 or R5 fires or julio asks | §5.3 | coordinator | 27B 2.3–2.6 h extrapolated from `wall_s` |
| 3 | GSM8K arm (`closed`, `store`/`tool`, 5 sizes) | H2, §5.7 | Sonnet (code, stats), coordinator (GPU) | **not measured**; no estimate exists |
| 4 | `funes` rows, one per size (D1) | the "deployed agent" claim, taint cost | coordinator; Sonnet for the stats | about 6 h (notebook, "julio: decide…" line; E0 measured 85 min per repeat at 9B) |
| 5 | A request-level capture of the tools block as served, or accept the GGUF template hash as sufficient | §5.3 mechanism wording | coordinator (needs a loaded model) | **not measured** |
| 6 | E2 (conflicts) and E3 (when to look up) | H3, §5.8 | Sonnet (code), coordinator (GPU) | **not measured** |
| 7 | Funes skill suite and size sweep with the store | H7, §5.9 | Opus designs, Sonnet codes | **not measured**; the suite does not exist |
| 8 | Run-to-run noise outside greedy: a second server/hardware, or temperature > 0 repeats | §7 limitation | coordinator | **not measured** |
| 9 | A second model family at 2–3 sizes | §7 limitation, H8 | julio decides | **not measured** |
| 10 | 27B `web` full cell and `web+recall-empty` control | §5.5, §5.6 | **needs a written search budget approved by julio** (Parallel credit is spent) | cost per search **not measured**; the 27B cell needs ≈ 2,100 searches (measured searches per searched question 2.2) |
| 11 | 20,000-resample H1 interval and a bootstrap that resamples sizes' dependence | §5.1 | Sonnet | no GPU |
| 12 | Knowledge-removed models (E1b/E6/E5): ROCm torch check, reproduction hours | §5.10 | Sonnet (script), coordinator (read-only yoda + GPU window), Opus + julio choose the route; **starts 2026-11-18 after midday** | **not measured** |
| 13 | Hand-label check of the abstain regex with more than 50 answers and a second labeller | §4.4 | julio | 50 labels exist; second labeller does not |
| 14 | Verify the power cap, thermal state and driver versions reported in §8 | §8 | coordinator | `[UNVERIFIED]`, see §8 |

### 0.4 Citations
Only papers on the plan's alphaXiv-verified reading list (2026-11-14) are cited. On 2026-11-17 this session re-found these on alphaXiv by title and read their abstract line: 2505.15962, 2607.07707, 2607.00605, 2606.26807, 2404.11672, 2605.29075, 2603.17781, 2610.25425. The others on the list (2607.12831, 2411.13504, 2212.10511, 2202.05262, 2210.07229, 2608.14290, 2510.02091, 2510.03366, 2503.23084, 2609.35312) are cited by title and arXiv id from the plan only, with no characterisation beyond the title; **before submission each needs a full read** `[TBD: Sonnet or Opus reads each paper's text via alphaXiv and rewrites §2]`. Yang, Gao & Wu 2507.18178 is *not cited* (the plan says it is not verified).

---

## Abstract

Small language models answer long-tail factual questions badly, and bigger models answer them better mostly because they store more facts. We ask how much of a model is left for everything else if the facts live outside it. We run one model family (Qwen3.5, 0.8, 2, 4, 9 and 27B parameters, 4-bit quantised, with 8-bit controls up to 9B) on 1,000 unambiguous PopQA questions in a fixed harness, with the facts in the weights (`closed`), in the prompt (`oracle`, `store`), reachable by a tool the model may call, or on the web. Giving the model its facts cuts the accuracy-versus-size slope to 0.27 of the closed-book slope (95% CI 0.21 to 0.35), and the 0.8B model with a local store (0.921) beats the 27B model closed-book (0.431) by 0.490 points (CI 0.457 to 0.523). The tool route is harder: whether a model calls the lookup tool when the prompt only says "use the tools when they help" does not follow size (lookup rate at 0.8, 2, 4, 9, 27B: 0.000, 0.000, 0.875, 0.001, 0.887), and a harness contract that forces one lookup is what makes the small models usable (9B: 0.347 without, 0.991 with). We could not show that forcing the lookup costs no accuracy where the model already looks up (equivalence within two points not shown, pooled p = 0.135). `[TBD: one sentence on the wording sensitivity of the 9B skip, from VARIANTS_TABLE.md, under whichever branch of App. A survives]`. `[TBD: H2, GSM8K: whether the same store leaves the size gain on fact-free reasoning intact]`. `[TBD: H3/E2 and E3 results]`. `[TBD: H7: whether a model within 8 GB with the store is within five points of the 27B on a skill suite]`. `[OPEN: sentence on knowledge-removed models, only if the route is run and reported]`. The negative and exploratory results are reported as such.

---

## 1. Introduction

A language model's parameters carry two things at once: how to do things (follow an instruction, call a tool, add two numbers) and what is true (who directed this film, which country this village is in). The second is expensive to store in weights, stale the day after training, impossible to inspect or edit, and the part a small model is worst at. A deployed agent can keep facts in three places: in the weights, in a local store the agent queries (a database of what the user and the agent have recorded), or on the web. This paper measures what each placement buys and what it costs, in a working agent harness, across five model sizes of one family.

The question behind it is an engineering one: if the facts are somewhere else, can a smaller model do the rest of the job in less video memory? We answer the part that inference alone can answer. We hold the question set, the prompts, the decoding and the serving flags fixed; we vary model size, quantisation and where the fact lives; and we report every comparison paired, on the same 1,000 questions.

**Contributions.**
1. A size curve in which the only thing that changes between conditions is where the fact lives (§5.1): the store slope is 0.27 of the closed-book slope (IQ4_XS) and 0.21 (Q8_0), and the smallest model with the store is above the largest model without it.
2. A measurement of what the tool route adds and what it hides (§5.3–5.4): with a lookup tool and a neutral prompt, accuracy depends mostly on whether the model decides to call the tool, which is not a function of size in our data, and a harness-side contract (the named tool must succeed before an answer is accepted) removes that dependence at a cost we could not bound to two points.
3. A description of the outside-content price: web search is far below the local store (§5.5), a write-back cache did not recover it (§5.6).
4. `[TBD: the cost table of §6 with VRAM, latency and taint, once the `funes` rows and the VRAM capture exist]`.
5. A harness, a hash-locked model list and every per-question result, enough to rebuild each table without a GPU (§8).

What this paper does *not* claim: that a 0.8B model "is" a 27B model with a database, that any result here is a law of size, that the findings carry to other model families, or (§5.10) anything about models whose pre-training removed facts.

## 2. Related work

*(Status: abstract-level reading only for the papers named in §0.4; to be rewritten after a full read.)*

**Models that externalise knowledge during pre-training.** Limited Memory Language Models (LMLM, arXiv:2505.15962) pre-train with factual knowledge moved out of the parameters into an external database that the model queries; Co-LMLM (arXiv:2607.07707) externalises knowledge with continuous queries and reports the goal of higher performance at smaller scales; a related audit studies forgetting in LMLMs (arXiv:2607.00605). Knowledge Offloading (KOFF, Galliamov, Choenni & Titov, arXiv:2605.29075) decomposes a model into a sparse backbone and memory modules. Knowledgeless Language Models (arXiv:2607.12831), KARLA (arXiv:2606.26807), Mixture-of-Memories (arXiv:2610.25425) and Intern-S2-Mobius (arXiv:2608.14290) are the other recent members of this line. These works change the model. Ours does not: we take released models as they are and change where the fact is. The distinction matters for what can be claimed (§5.10).

**Explicit memory and agent memory.** MemLLM (arXiv:2404.11672) fine-tunes a model to use an explicit read-write memory. Knowledge Objects (arXiv:2603.17781) benchmark in-context memory against structured fact objects for persistent LLM memory. Agent-memory work in general compares memory designs; we vary the *model size* under a fixed memory.

**Retrieval, popularity and when to look up.** PopQA (Mallen et al., arXiv:2212.10511) showed that parametric recall fails on the long tail of entity popularity; we use it as our data and report accuracy by popularity bucket. Adaptive retrieval and Self-RAG-style work asks when a model should retrieve; our lookup-rate results (§5.3) measure when it does under a given prompt.

**Knowledge conflicts and editing.** CounterFact-style editing (ROME and MEMIT, arXiv:2202.05262, 2210.07229) and MemoReason (arXiv:2609.35312) motivate the planned conflict experiment (§5.8, not yet run).

**Memory versus reasoning, by layer.** Jin et al. (arXiv:2411.13504) and the layer-localisation papers (arXiv:2510.02091, 2510.03366, 2503.23084) separate recall from reasoning inside the network; they bear on the optional layer-ablation track (§5.10).

**Gap.** The plan's claim of novelty was that nobody has the three-way comparison inside a working agent and nobody counts the safety cost of outside content. `[TBD: re-check this claim against the literature before submission; this session did not search for competing work beyond the plan's list]`. Note that our E1 harness is a controlled harness, not the production agent loop (§3.5): the three-way comparison inside a deployed agent currently rests on E0 only (§5.5).

## 3. Setup

### 3.1 Questions
PopQA (Mallen et al.) test set, from which we keep the **unambiguous** items: a (subject, property) pair that has exactly one candidate object over all 14,267 triples in the file (E0: of 1,000 random items, 142 were ambiguous; every hedged answer in E0's `store` condition, 81 of 142, was on homonym subjects; `NOTEBOOK.md`, D5). A sample of **1,000** is drawn stratified by popularity bucket (dataset seed 1). Bucket counts: 1e0: 16, 1e1: 195, 1e2: 195, 1e3: 195, 1e4: 195, 1e5: 194, 1e6: 8, 1e7: 2 (`E1_TABLE.md`). Buckets of 16, 8 and 2 questions are not read.

### 3.2 Models
Qwen3.5 0.8, 2, 4, 9 and 27B, `unsloth` GGUF files, **IQ4_XS** at every size (main curve) and **Q8_0** at 0.8, 2, 4, 9B (control curve). Every served file is tied by `models.lock` to its repository, file name, size and sha256, and the runner refuses a file whose bytes do not hash to the lock entry (`ko/models.py`). 27B Q8_0 is 28,595,763,104 bytes and does not fit the 16 GB card, so the control curve has four points (D11, notebook). The chat template of all eight 0.8–9B files is byte-identical (sha256 `7f0e529032c25183bcd66c7f238da2d377f43be754a94e2725a58c4e16d2ed67`, 7,816 characters); the 27B file's template differs by one character (`e60df41481b6ad20571cda7e2e1290cfee25670f1b50973f74576c9354c66336`, 7,817) (notebook, coordinator entry "template route (i)"). Serving flags are identical for every size: llama.cpp `llama-server`, `--jinja --reasoning-budget 0 --chat-template-kwargs enable_thinking=false`, context 8,192, one slot, f16 KV cache, no prompt cache (`scripts/ko_swap.py`; notebook). Each request carries `enable_thinking: false` and `tool_choice: auto`; temperature 0; `max_tokens` 128.

*Lineage note (a correction we made, kept in the paper's record).* Our first pilot (E0) used a nominally Qwen3.8 9B file that turned out to be a community distillation; its numbers are reported only as E0 and never mixed into the size curve (`models.lock` records it as `community-distill (inferred)`).

### 3.3 Conditions
All conditions answer the same question with the same one-sentence system prompt (`SYSTEM`: "You answer factual questions. Reply with only the answer: a short phrase, with no explanation and no full sentence.") unless stated.

| condition | where the fact lives | what the model does |
|---|---|---|
| `closed` | the weights | answers |
| `oracle` | the prompt | the question's own triple is given to it |
| `store` | a local store, retrieved by the harness | BM25 over all 14,267 triples; top results are put in the prompt |
| `tool` | a local store, queried by the model | a `lookup` tool it may call; default prompt = `SYSTEM` + "Use the tools you are given when they help." (`SYSTEM_TOOL`); up to 4 steps |
| `tool+contract` | same | `tool`, plus the harness refuses a text answer until `lookup` has succeeded; at most 2 nudges ("You have not used {tool} yet. Call it before you answer.") |
| `web` | the web | a `web_search` tool (Parallel's search, keyless MCP; Tavily as a fallback) |
| `web+writeback` | the web, then a local cache | after each answer that used `web_search`, the harness stores "Q … A … (per url)" as tainted; a second pass repeats the questions |
| `funes` | the deployed agent's memory | the production agent loop on a scratch copy of its database (E0 only so far) |

Prompt variants: `search_first` (the prompt tells the model to look up first; used for E1's tool cells), the default prompt above (used for H5/H8), `abstain_ok` / `abstain_default` (the default tool prompt plus an abstention sentence, ablated arm only, D16), and the three pre-registered default-style variants `tool_first`, `tool_free`, `tool_describe` (§5.3, App. A). **The `store` condition is at ceiling by construction** (§7): the PopQA questions are templated from the very triples that the store holds.

### 3.4 The ablated arm
The first 200 sample questions again, with the question's own triple removed from retrieval (one filter, no second store). Outcomes: *correct* (answered from the weights), *abstained*, *hallucinated* (a wrong answer). This arm exists for H5b.

### 3.5 Funes as the testbed
Funes is a self-hosted agent whose memory is a SQLite store with vector search, full-text search and links. The experiments here do **not** run the full agent loop except the `funes` rows; they use a purpose-built harness (`benchmarks/knowledge-offload`) so that every condition differs in one thing. `[TBD: one paragraph and a diagram on Funes, per the plan; only after the `funes` rows exist, since without them the paper cannot honestly call E1 an agent measurement]`.

### 3.6 Hardware and windows
One AMD RX 9060 XT (16 GB, ROCm) on the machine that also serves the production model; each size was served in a swap window, with the production 27B stopped and restored after every block by `scripts/ko_swap.py`, which refuses to start while a cron job is due and ends a run 17 minutes before the next window. The model is never run on a laptop CPU. Measured run times of the 36-cell default-prompt job: queue 11:26 to 18:44 on 2026-11-17, 7.3 h wall, 4.91 h of cell time (`results/queue_h5h8.log`).

## 4. Methods

### 4.1 Primary metric and its hierarchy
**`contains` on the 1,000 unambiguous items** is primary: the answer, normalised, contains the gold object. `exact` and `contains ∧ ¬hedge` (a hedge is an answer that names two or more candidate objects of the same subject) are secondary; on this sample the three coincide to within the table's precision, because unambiguous items rarely admit a list. The choice was made after seeing that on the full, ambiguous sample `contains` credits a list while hedge-adjusted scoring penalises it, so comparing conditions there compares how they handle ambiguity, not knowledge (D5; E0: `store` .980 hedge-unadjusted vs .905 adjusted). Ambiguous items are reported separately and are exploratory (E0: 142 items).

### 4.2 Paired design and uncertainty
Every condition answers the same questions, so comparisons are paired: McNemar (exact) for accuracy differences, a paired bootstrap for the size of a difference. All intervals are 95% percentile bootstraps over questions, 10,000 resamples, and one resample is shared across sizes and conditions so that every difference and slope is paired (`stats_e1.py`; E0's tables used 2,000 resamples). One run of each cell: greedy decoding is deterministic here — two repeats of the five harness conditions were bit-identical in E0 (0 of 1,000 predictions differ) and a 100-item pilot at the 9B IQ4 for three conditions found 0 of 100 predictions different (notebook). Only `funes` varies, because the agent runs at temperature 0.2. **We therefore report no run-to-run variance for the main cells; the intervals are sampling intervals over questions only** (§7).

### 4.3 Hypotheses and the Holm family
H1–H8 were written in the plan and committed before the E1 sweep (plan, "Hypotheses, stated before running"; reworded where noted by date in the plan and by decision numbers D1–D16 in the notebook). The confirmatory family is H1, H2, H3, H4, H5 and H7 (m = 6); H6 and H8 are exploratory and outside it. H2, H3, H4 and H7 have no data, and enter Holm as p = 1; so every adjusted p-value in this paper is a **conservative** bound and will be recomputed when those tests exist. Equivalence tests (H5a) use TOST with a ±2 point margin; equivalence is declared only when the TOST p is below the Holm threshold.

### 4.4 The abstention outcome
Abstention is decided by a regular expression (`ko/ablate.py`). Its agreement with 50 hand labels, half of which the regex called abstained, is **49 of 50 (98.0%)**; the one disagreement is a `closed` answer ("Oregon") the regex called hallucinated and the human called correct (`results/abstain_labels.tsv`; `E1_TABLE.md`, `H5H8_TABLE.md`). An earlier claim of 50/50 was circular (the regex had been tuned on the same answers) and was withdrawn (notebook 9e20a44). The check is one labeller on 50 answers; `[TBD: second labeller, 200 answers, item 13 in §0.3]`.

### 4.5 Validity rules
A cell whose tool backend failed is marked INVALID and excluded from every difference, slope and bucket (`ko/validity.py`, `results/invalid_cells.json`). The harness stops a cell on any failed search (exit 4) and refuses to write it. Data and decisions that changed after seeing results are labelled as such in the notebook; any analysis added after seeing results is called *exploratory* here.

### 4.6 Search backends and the search budget
`web` uses Parallel's keyless search MCP (five excerpts per search, about 3.5 kB each as measured in the first probe, cut to 600 characters per excerpt in the model's view) with Tavily as fallback. In E1 every search of every valid web cell was served by Parallel. Parallel's USD 20 welcome credit ran out during the 27B cell: **1,661 of its 2,086 searches returned `402 Insufficient credit`** (`WEB_AUDIT.md`); Tavily's remaining quota is reserved for production Funes, so no web cell is rerun until julio approves a written budget.

### 4.7 How the work was organised: sessions, roles, rules and files

The experiments were designed, coded, run and analysed by a set of language-model sessions (Claude Code) working under one human, julio, with **no shared chat context**: a session knew only what was in the repository and in the brief it was given. This subsection describes the protocol so that it can be repeated and criticised. Every statement cites a file, a commit or a notebook entry on branch `feat/knowledge-offload-e0`; nothing here comes from memory. Evidence is the notebook (`benchmarks/knowledge-offload/NOTEBOOK.md`, 2026-11-15 to 2026-11-17), `git log` (55 commits touching `benchmarks/knowledge-offload` or `docs/knowledge-offload-plan.md`) and the plan.

**Roles (plan, "Which Claude for which part"; notebook "Who does what").**

| role | model · effort | what it did on the branch | forbidden |
|---|---|---|---|
| coordinator (julio's main thread) | model-driven session `[TBD: model and effort of the coordinator are not recorded in any committed file; the coordinator or julio states them]` | assigns tasks, starts and stops sessions, launches GPU jobs in a window julio chooses, writes briefs, relays messages (e.g. the template read, the variants queue decision, entries headed "coordinator") | doing another role's task silently |
| design / audit | Opus 5.5 · high | pre-registrations, decisions D5–D16, the lineage error report, the audit of all sessions, the 9B variant design; **no code, no GPU** | code, models, yoda |
| code and statistics | Sonnet 5.5 · medium | harness (`run.py`, `ko/`), `ko_swap.py`, `models.lock` gate, stats scripts, queue and verifier scripts, tests (hermetic, 141 passing at the variants commit) | touching yoda, taking a task assigned to another |
| early review | Fable 5.1 · high | `DECISION_BRIEF.md` and decisions D1–D4 (entry 2026-11-15 "pre-E1 decisions") | code |
| GPU | **no model** | only a job launched through `scripts/ko_swap.py` in a window julio chose; the script refuses a second swap, refuses a cron window, and restores the production model in a `finally` block | a session starting, stopping or swapping a model on its own |
| the human | julio | scope and budget decisions (Parallel credit, Tavily reserved for production, whether knowledge-removed models enter), approval of the three variant wordings, **the 50 hand labels** (`results/abstain_labels.tsv`, dc5d669), the windows | – |

Why the split: the plan assigns the model to the part where an error is expensive (design, where "a reasoning mistake costs weeks") and keeps the part that must be reproducible free of a model ("a run that needs a model to babysit it is a run that isn't reproducible"). The rule that only the coordinator touches yoda exists because the GPU is shared with the production model and a swap has side effects (probes paused, guardian timer stopped, both to be undone: notebook "yoda run state").

**Rules of work (notebook, section "Rules of work", julio, commit 017185e, 2026-11-17 09:20).**
1. *Golden rule: nobody assumes anything in order to move on; everything is measured.* A number in a decision, report or commit message has a source the writer ran; "done" is checked against the real state, never against another session's report; estimates stay marked "estimated" until a run replaces them; if a measurement is impossible now, say so and stop on that point.
2. One task per session; tasks are assigned by the coordinator; a session that finds a task done or open reports it and stops.
3. One branch. `git pull --rebase origin feat/knowledge-offload-e0` before every commit, push right after, never force, commit only your own files; results JSON and tables are committed, `results/payloads/`, `*pilot*`, `*smoke*` are not.
4. Each session reads the notebook's "Now" block and its last three entries at the start, and at the end updates "Now" and adds a dated entry containing *what was measured (with the command)*, *what was claimed but not measured*, *what it leaves open*.
(Before the rules were written the practice was informal; two of the failures in the list below predate them.)

**Channels: what carried information between sessions, and what did not.**
- **The plan** (`docs/knowledge-offload-plan.md`): hypotheses, experiments, rules; its commit date is the pre-registration. Edited by the design role and by a code session only "to record a result".
- **The notebook**: a "Now" block (state, what is not run, what is invalid, pending decisions) and dated entries, each with a header *date · step · topic · role · model · effort*; 28 entries to 2026-11-17. Decisions carry identifiers **D1–D16** (D1–D4 Fable, D5–D8 and D9/D10 Opus, D11–D16 Opus after E1; D9 was superseded by D10 the same day and says so). The coordinator's instruction names a D17; **none exists on the branch** `[TBD: coordinator confirms whether D17 is unwritten]`. Code sessions end their entry with "Open for Opus"; the design role answers those items in its own entry (audit entry, "Answers to Sonnet's 'Open for Opus'"), which is how a question crossed from one session to another without a shared context.
- **Results as files**: one JSON per cell (`results/popqa-stratified1u-*.json`, 128 committed), recording model id, quantisation, commit, seed, date and endpoint; tables that list the **sha256 of every input file** (`H5H8_TABLE.md`, "Input files (sha256)"). Tables are regenerated by a script from the JSON, never typed.
- **`models.lock`** (repo, file, size, sha256, quant, lineage per model) and the runner gate that refuses a served file whose hash is not in it (commit 3cd759b): the model's identity is a file, not a name.
- **Queue scripts and logs** (`results/queue*.sh`, `queue*.log`, `ko_swap-*.log`): what was run, in which window, with which retries; scripts are resumable, take a `DEADLINE`, and have a `DRY=1` mode.
- **`verify_*.py` as the gate between a run and its analysis** (`verify_h5h8.py`: 36 of 36 cells before `stats_h5h8.py` was run; `verify_variants.py`: exit 0 only with 12 of 12). A missing cell prints "missing", never a number.
- **Briefs and messages**: a session received a brief at start and, between sessions, cross-session messages. **Neither is on the branch.** What survives of them is their effect in a notebook entry; consequently, anything a session needed had to be written into a file, and anything only said in a message was lost to the next reader. `DECISION_BRIEF.md` is the one brief-style document that is committed.
- **Not shared**: the chat context of every session. An audit session could not read others' tool arguments; it used the notebook, commits, `git status` and the result JSON as evidence (audit entry, "Sources").

**What went wrong, and what the files caught or repaired.** Counted from the notebook; none softened.
1. **Wrong model lineage (E0).** E0 ran on a community distillation labelled "Qwen3.8 9B" in the plan, tags and notebook; about 6 of about 12 GPU hours were wasted (about 2 h of deterministic repeats and about 3 h of `funes` repeats that D1 had already judged unnecessary were avoidable regardless; the rest is the error itself). It **was not caught by the protocol**: `--expect-model 9B` compared a name, no result recorded a hash, and the review sessions "reasoned from the harness's labels". It surfaced because julio asked what to download. Repair: `models.lock`, a sha256 gate, hash/repo/lineage in every cell's metadata, "one curve = one lineage = one quant producer", and a rule that the first review item for any model is "what is this file" (notebook "ERROR REPORT", 2026-11-16; 3cd759b). One more consequence of the same assumption: the design role's own D9 repeated it for a day before D10 superseded it.
2. **A circular check reported as validation.** The abstain regex was reported as agreeing 50 of 50 with labels (01b461a, 2026-11-17 08:55); the labels were not independent. Withdrawn 26 minutes later (9e20a44, "human labels still empty"); julio hand-labelled 50 answers: 49 of 50 agree (dc5d669). The earlier E1 table carried the withdrawn number for that interval.
3. **A silently invalid cell.** The 27B `web` cell ran on an exhausted search account: 1,661 of 2,086 searches returned a 402 error that the harness's `tool_loop` turned into tool text, so no error was recorded. An E1 notebook entry first reported the 27B's lower `web` accuracy as a finding ("not monotone in size"), with the caveat that it needed the payloads read. A payload audit (`web_payload_audit.py`, 5a940c0) found the failures; the harness was changed to stop a cell on any failed search (exit 4; ed0ac4f) and the cell is flagged INVALID in every table.
4. **Statements not repaired at their original line.** The audit found four contradictions fixed only in later entries, and a "Now" block that still described the default-prompt cell as "not run" after D14 (audit entry, (b) and (c)); the notebook is append-only by convention, so a reader must read to the end.
5. **Rule slips, disclosed by the sessions themselves:** (a) a code session ran `git fetch` in the yoda checkout, refs only, and said so; (b) the design session ran the E1 queue from its own session with julio's authorisation and caused a 10-minute idle gap (16:21–16:31), stated in the entry header; (c) the first 27B load test reported "does not fit" because the prompt was 24.9k tokens against an 8,192 context (fixed in 2220cc2; a `ko_swap run` option was also swallowed, fixed in 06f34d3); (d) the variants code was written by the coordinator because a code session could not be started, which is a role deviation, recorded in that entry's header ("coordinator did it"). The audit entry (c) reports no violation after the rules were written that its author could find; its author did not inspect yoda.
Total: three errors that touched published numbers or GPU hours (items 1–3), one set of four stale or contradicted statements (item 4), and four disclosed slips (item 5, a–d). Only the lineage error has a measured cost (about 6 GPU hours); the cost of the others was not measured.

**What the arrangement can and cannot support (limits).**
1. **Authorship per session is not in git.** Every one of the 55 commits carries the same git author (julio's identity). Commit trailers name a *model* (46 `Claude Sonnet 5.5`, 9 `Claude Opus 5.5`), not a session or a role; attribution to a role comes only from the notebook headers, which are self-reported.
2. **The coordinator is itself a model.** Its reports were not trusted: the audit re-derived lookup rates, nudge counts and accuracies from the committed JSON and found them equal to `H5H8_TABLE.md`, and did not inspect yoda itself; the yoda-side facts it could not see (checkout HEAD, restored services) it lists as "not measured".
3. **No independent human review of the code is recorded on the branch.** The checks are the hermetic tests, the verifiers and cross-reading between sessions; they are checks by models of models.
4. **One human made the scope and budget decisions** (whether to run web cells, the search budget, which wording of the abstention clause, whether knowledge-removed models enter, which windows), and the only independent labels are his 50.
5. **Messages and briefs are not archived**, so the protocol cannot be replayed from the repository alone; only its products can.
6. **The same family reviewed itself.** All sessions were Claude models; shared blind spots are not controlled for. The lineage error is an example of a blind spot shared by every reviewer.

**To repeat the protocol:** (i) put identity in a lock file and gate the run on it; (ii) write hypotheses and decisions with identifiers before the data; (iii) keep results as files with hashes and regenerate tables from them; (iv) put a verifier between a run and its analysis; (v) end every session by writing what was measured, what was only claimed, and what is open; (vi) assign GPU work only through a script that restores the production state; (vii) have a different session audit the notebook against the files before closing sessions.

## 5. Results

All numbers in this section are measured, from the files named. Intervals are 95%.

### 5.1 Supplying the facts flattens the size curve (H1)

*Source: `results/E1_TABLE.md` (sha256 `a0fd650c5f68c63a459b65d00d67be471c24ba528b8d0e9d79734dfa94436771`); n = 1,000, `contains`, IQ4_XS.*

| | 0.8B | 2B | 4B | 9B | 27B |
|---|---|---|---|---|---|
| `closed` | 0.213 [0.188, 0.238] | 0.243 [0.217, 0.270] | 0.320 [0.292, 0.349] | 0.350 [0.321, 0.379] | 0.431 [0.400, 0.461] |
| `oracle` | 0.972 | 0.995 | 0.997 | 0.987 | 0.997 |
| `store` | 0.921 [0.904, 0.937] | 0.971 [0.960, 0.981] | 0.984 [0.976, 0.991] | 0.980 [0.971, 0.988] | 0.991 [0.985, 0.996] |

- Slope of accuracy against log10(parameters): `closed` +0.146 per decade [0.127, 0.165]; `store` +0.040 [0.031, 0.049]; ratio **0.27 [0.21, 0.35]** (Q8_0: 0.21 [0.13, 0.31]); the pre-registered threshold (ratio < 0.5, upper bound below 0.5) is met on both quantisations.
- **0.8B + `store` vs 27B `closed`** (IQ4_XS): 0.921 vs 0.431, Δ = +0.490 [+0.457, +0.523]; 510 questions only the 0.8B-with-store got right, 20 only the 27B closed-book (McNemar p = 5.2e-124).
- `store − closed` per size shrinks with size: +0.708, +0.728, +0.664, +0.630, +0.560 (0.8 to 27B).
- By popularity: closed-book accuracy is lowest in the middle of the popularity range (e.g. 9B: 0.118 at bucket 1e2, 0.773 at 1e5) and the store lifts every bucket to 0.96 or above at 4B and up. Buckets 1e0, 1e6, 1e7 are too small to read.
- **Holm.** H1 raw p ≤ 1e-4 (the bootstrap's resolution); Holm-adjusted (m = 6, four tests with p = 1) 0.0006 (`results/HOLM_TABLE.md`, sha256 `a731f5ea4c6ec5643bdb3815739176ee92f49b891f3c917845251e9b2ea5af6e`). **H1 on Q8_0 is not testable in full** (no 27B Q8_0 cell), so its H1b half is p = 1 there.

*Caveats that belong to this result.* (i) `store` is at ceiling by construction (§7). (ii) `oracle − store` is the reading-and-retrieval cost and shrinks with size (+0.051 at 0.8B, +0.006 at 27B), so part of the `store` slope is retrieval, not knowledge. (iii) The slope is fitted on five points of one family. (iv) The CI on the slope resamples questions only.

### 5.2 What the store costs: reading capacity
At the smallest size, giving the model the exact triple (`oracle` 0.972) is better than letting the harness retrieve it (`store` 0.921; difference +0.051 [0.035, 0.067], 60 vs 9 discordant). From 9B up the difference is within noise (9B +0.007 [−0.003, +0.017]; 27B +0.006 [0.000, +0.013]). Pre-registered handling: a gap above 10 points with retrieval fine would have been reported as a reading-capacity limit; the observed gap is 5.1 points. `[TBD: retrieval_hit on this sample, from the E1 cell metadata; the kill criterion was retrieval_hit < 0.9; the number is in the cell files but was not copied into E1_TABLE.md, so it is not quoted here]`. `[TBD: E4 store shape, only if retrieval turns out to be the limit; no evidence of that so far]`.

### 5.3 When the model looks up on its own (H8, exploratory)

*Source: `results/H5H8_TABLE.md` (sha256 `ed1a2f8af2e8cf5091dda78a80e390bc1629fc2842940d7f2043dc8738e376a2`), default prompt, 1,000 questions per cell.*

| lookup rate, `tool`, default prompt | 0.8B | 2B | 4B | 9B | 27B |
|---|---|---|---|---|---|
| IQ4_XS | 0.000 | 0.000 | 0.875 | 0.001 | 0.887 |
| Q8_0 | 0.006 | 0.000 | 0.574 | 0.004 | – |

- **H8 as pre-registered (lookup rate rises with size) is not supported.** The slope is +0.485 per decade [0.474, 0.496] (IQ4_XS) and +0.153 [0.144, 0.163] (Q8_0), but the profile is non-monotone: the 9B is at 0.001 between the 4B (0.875) and the 27B (0.887). A slope through these points describes the fit, not a size law, and we do not report it as one.
- When a model does not call the tool, its accuracy equals its own `closed` accuracy (9B IQ4: 0.346 on 999 questions; `closed` 0.350) and its answers are short (E0/H5H8 audit: mean 3.9 completion tokens, no tool-call markup in any prediction at any size, measured in the notebook audit entry).
- Under `search_first` the same 9B looks up 0.898 (IQ4_XS) and 1.000 (Q8_0) (`E1_TABLE.md`): the tool path works for this model, so the default-prompt skip is not a broken harness.
- The 4B and the 27B skip most when the entity is popular: 4B IQ4 lookup 190 of 195 at popularity bucket 1e1 and 101 of 194 at 1e5 (computed from the cell files, notebook audit entry). The decision is partly calibrated to what the model knows.
- Quantisation changes it: 4B IQ4_XS 0.875 versus Q8_0 0.574; of the 4B's shared lookups, 301 questions were looked up only by IQ4 and none only by Q8.
- The chat template of the 0.8–9B files is byte-identical and the 27B differs by one character (§3.2), and the serving flags are identical across sizes; so the difference between 4B and 9B is not a template or flag difference. What remains is the weights and the prompt. **Not excluded:** the server's own tool-call parsing (same binary for all) and the one-character 27B template difference.

**Pre-registered variants (stages A and B).** To test whether the 9B's skip is a property of the model or of one wording, three variants of the default prompt were fixed before any run (`NOTEBOOK.md`, entry "step 2 of the 9B plan", approved by julio and frozen): `tool_first` (order of the two sentences swapped), `tool_free` (the format sentence removed), `tool_describe` (the tool is described, not ordered). Stage A: 9B and 4B IQ4_XS. Stage B: 4B and 9B Q8_0. Stages C (27B) and D (2B) run only under rule R3/R5 or on julio's word.

`[TBD: VARIANTS_TABLE.md (not yet generated; `stats_variants.py` produces it): lookup rate per cell with CI, accuracy, accuracy by looked-up/not, completion tokens, empty answers, share at the token limit, paired agreement with the baseline cell]`

**What this subsection will say is written in App. A, one branch per pre-registered reading (R1 to R5, and "none"), written before the table exists. One branch will be kept; the rest deleted, and each deletion logged in the notebook.**

### 5.4 Forcing the lookup: the contract (H5)

*Sources: `H5H8_TABLE.md`; `HOLM_TABLE.md`.*

- **Accuracy (answerable questions).** Without the contract, the default `tool` accuracy is 0.191 / 0.245 / 0.975 / 0.347 / 0.979 at 0.8 / 2 / 4 / 9 / 27B (IQ4_XS), which is the lookup rate's shadow; with the contract it is 0.190 / 0.374 / 0.991 / 0.991 / 0.996. The contract makes the 9B go from 0.347 to 0.991 (+0.644 [0.615, 0.674], 644 vs 0 discordant). At 0.8B it does nothing, because the model never calls the tool even when nudged (lookup rate 0.000). At 2B IQ4_XS it works for a fifth (0.212) of the questions only; at 2B Q8_0 it works for 0.958.
- **H5a (equivalence within ±2 points where the model already looks up): not shown.** Eligible sizes are those where the contract's lookup rate is ≥ 0.99 and the plain tool's is ≥ 0.8 (D12): IQ4_XS 4B (+0.016 [+0.009, +0.024], 16 vs 0 discordant, TOST p = 0.185) and 27B (+0.017 [+0.009, +0.025], 17 vs 0, TOST p = 0.26). Pooled IQ4_XS: +0.017 [+0.011, +0.022], TOST p = 0.135; Holm-adjusted 0.676. For Q8_0 **there is no eligible size**, so H5a cannot be computed there. In words: where the tool is already used, forcing it gained about 1.7 points, in the same direction at both sizes, and our data neither show equivalence within two points nor a difference we could assert after correction. Equivalence at this margin needs more power than 1,000 questions at these accuracies give.
- **H5b (hallucination on absent facts, ablated arm, prompt with the abstention clause).** The contract lowers the share of wrong answers at the sizes where the plain tool did not look up: 2B IQ4_XS −0.270 [−0.340, −0.200] (exact sign test p = 2.59e-13); 9B IQ4_XS −0.325 [−0.405, −0.245] (p = 9.04e-13); 9B Q8_0 −0.280 [−0.360, −0.200]; 2B Q8_0 −0.125 [−0.185, −0.065]. Where the plain tool already looked up (4B, 27B IQ4_XS) the difference is nil (−0.005 at both). Pooled over sizes: IQ4_XS 167 discordant, 21 contract-more vs 146 tool-more, exact p = 3.12e-24; Q8_0 146 discordant, 28 vs 118, p = 2.4e-14. **These pooled p-values treat the same 200 questions at five sizes as independent strata and are optimistic**; H5's Holm entry is the *maximum* of H5a and H5b (0.135 for IQ4_XS), which is why H5 does not survive correction (Holm 0.676).
- **What it means.** The contract's effect on hallucination is the effect of making a model that would not look up do so. Where the model looks up on its own, the contract changes nothing measurable. This is a result about prompt-elicited tool use (§5.3), not about a property the contract has by itself.
- The earlier E1 table (prompt `search_first`) shows the same at 2B IQ4_XS (accuracy 0.816 vs 0.959, +0.143 [+0.122, +0.166]); we report the default-prompt cells as the H5 test because D12 pre-registered them.

### 5.5 Web as the store

*Sources: `E1_TABLE.md`, `WEB_AUDIT.md` (sha256 `f42815287233ebd327e0d0165a764069f08849007e2a810b61da8f6dcd276883`).*

- `web/search_first`, `contains`, n = 1,000: 0.8B 0.436 [0.405, 0.467]; 9B 0.642 [0.612, 0.671]. Against `store` the loss is −0.485 [−0.519, −0.451] at 0.8B and −0.338 [−0.368, −0.308] at 9B; against `closed` the gain is +0.223 [+0.191, +0.256] and +0.292 [+0.261, +0.324].
- **The 27B `web` cell is INVALID** (1,661 of 2,086 searches failed with a credit error). The first failed search was at question 446 in sample order; every one of the 527 questions searched from there on had at least one failed search. We report the 27B only on the **424 questions whose searches all succeeded**, labelled partial and not comparable to a full cell: 27B 0.748 against 9B 0.623 on the same 424 questions (`WEB_AUDIT.md`). We do not stitch search backends inside a cell.
- *Why web is below the store, measured, not explained.* When the gold object was in the excerpts the model saw, the answer was right 0.765 of the time at 0.8B (362 questions) and 0.761 at 9B (502 questions); when it was not, 0.000 and 0.054 (109 and 184 questions). The model's view cuts excerpts at 600 characters; the gold was in the full excerpts but cut away in 48 (0.8B) and 70 (9B) of the searched questions. The search tool's yield, not the model's reading, is the first limit. `[TBD: rerun at 2B/4B and the full 27B cell; needs the search budget, item 10]`.
- `[TBD: E0 comparison, funes@3000 vs store, only as an E0 side note, with its lineage caveat (community-distill 9B)]`. For the record, E0 (distill, not part of the curve): `funes@3000` 0.912 vs `store` 0.992 on 858 unambiguous items (`E0_TABLE_unambiguous.md`, sha256 `3df7a962aed1aca12ba25c09fc42ec19c56eae7d48e097d138200a0476b23077`).

### 5.6 Write-back (H6, exploratory, negative)

H6 was demoted to exploratory after E0 (`web+cache` pass 2 − `store` = −0.403; the model-driven cache never formed, four `remember` calls in 1,000). With a harness-side write-back at 9B IQ4_XS: pass 1 is **below** plain `web` by −0.219 [−0.249, −0.191] (25 vs 244 discordant); pass 2 is above pass 1 by +0.039 [+0.023, +0.055] but still below plain `web` by −0.180 [−0.208, −0.153]. In pass 1 the first tool called was `recall` on 892 of 1,000 questions and none on 108, and the model searched far less than in plain `web` (180 searched questions, 181 searches, against 686 and 686; `WEB_AUDIT.md`). The pre-registered criterion "pass-2 accuracy ≥ pass-1 − 2 points and pass-2 web calls ≤ 10% of pass 1" is met on accuracy (+0.039) but not on calls (81 searches in pass 2 against 181 in pass 1: 45%, not ≤ 10%), so **H6 fails**. Wrong answers are cached too (by design). The write-back arm confounds the tool list (it adds `recall`) with the cache; the control `web+recall-empty` that would separate them was never run (code not written; D15(2)) `[TBD: decide whether write-back stays in the paper; if it stays, the control and a capture of what `recall` returned are needed]`.

### 5.7 Fact-free reasoning (H2)
`[TBD: GSM8K arm: `closed` and `store`/`tool` with the distractor-only store, five sizes; H2 = the `store` slope is not flattened (≥ ½ of `closed`); file: a future `results/E1_GSM8K_TABLE.md`; blocked on item 3 of §0.3]`. No statement about reasoning is made in this draft.

### 5.8 Conflicts and edits (H3) and knowing when to look up (E3)
`[TBD: E2: context-faithfulness and parametric override per size, logistic regression of override on log(params); supersede vs contradict in Funes]`. `[TBD: E3: lookup rate per kind of question (answerable, weights-only, unanswerable), abstention, accuracy when the store is wrong or noisy]`. What is measured today about abstention is only §5.4's ablated arm.

### 5.9 Does it fit a smaller model? (H7)
`[TBD: Funes skill suite (does not exist), size sweep with the store, H7: a model ≤ 8 GB with the store within five points of the 27B]`.

### 5.10 `[OPEN: depends on the route and its results]` Models with knowledge removed
This section is a placeholder for the optional second track: the question of whether pre-training or editing that removes facts from a model changes what its parameters do (LMLM and Co-LMLM against their standard twins; layer ablation; a lookup LoRA). **julio decided on 2026-11-17 that this line of work is wanted. The route, the order and the cost are not decided.** The checks that precede that decision (does the pinned torch run on the ROCm card, how many hours reproducing the published numbers takes) begin on 2026-11-18 after midday. Nothing is claimed here about those models. When the route is chosen, this section is rewritten from its own pre-registration; H4 (non-fact perplexity) belongs here, and enters the Holm family with its own p-value.

## 6. Cost
`[TBD: cost table per condition: VRAM (not recorded by the harness at present), latency (wall time per cell is recorded: e.g. baseline `tool` cells 77–2,703 s, `tool/search_first` cells 125–2,946 s, `search_first` is slower where it looks up; `H5H8`/`VARIANTS` for the rest), API cost (web: USD 20 credit consumed; cost per search not measured), taint rate (not computed for the E1 conditions)]`. The plan's contribution sentence promises VRAM, latency, privacy and safety-gating costs; **as of this draft only latency is measured**.

## 7. Limitations

Written early on purpose; each is a thing a reader should discount.

1. **One GPU, one machine.** All cells ran on one consumer card shared with a production model, in windows. Wall times are specific to it.
2. **One model family.** Qwen3.5 only. Findings about size are findings about this family's five checkpoints, not about "size". The 4B/9B contrast in lookup behaviour is the clearest sign that checkpoint-specific behaviour dominates.
3. **Two quantisations, and one is partial.** IQ4_XS on every size; Q8_0 as a control on four sizes. Quantisation changed lookup rates materially (§5.3), so size and quantisation are not independent in any cross-size statement.
4. **Greedy, one repeat.** Results are deterministic given the server; intervals are over questions only. We did not measure variance across servers, drivers or sampling.
5. **PopQA is templated.** The questions are generated from the same triples that populate the store, so `store` and `oracle` are at ceiling by construction (retrieval is trivial, and the store contains exactly the facts the questions ask). The H1 slope ratio is therefore about *supplying* facts, not about *finding* them in a messy store. The messy-store case is what the deployed agent has and what the `funes` rows and E4 would measure; neither exists yet.
6. **The metric is lenient.** `contains` credits longer answers that include the gold; hedging (a list of candidates) is a rare phenomenon on unambiguous items but not zero. The abstention regex agrees 49/50 with one labeller.
7. **The lookup tool is simple.** One tool, one schema, a local BM25 store. Different tool descriptions and schemas would change §5.3; the variants measure only prompt text.
8. **The harness is not the deployed agent.** Only E0's `funes` rows exercise the agent's own loop, and on a different (distilled) 9B.
9. **Search backend split.** Parallel only for all valid web cells; Tavily is reserved for production. The 27B web cell is partial (424 clean questions), and no web cell exists at 2B or 4B. Parallel's excerpt quality is a confound for every web number.
10. **Small LMLMs.** If §5.10 is run, the released LMLM models have 176M–382M parameters and complete biographies rather than follow instructions; they bear on whether masking changes the parameters, not on whether Funes could run on one.
11. **Post-hoc elements.** The sample, the metric hierarchy (D5), H5's reframing (D12–D14) and H8's reframing were decided after E0 and, for H5/H8, after E1; each is dated in the notebook. The Holm family fixes m = 6 with four empty entries as p = 1.
12. **Hash evidence.** The GGUF files are tied to the repositories by their LFS sha256 and verified by the runner; the template hashes were read from the file headers. We did not check the llama-server build's tool-call parsing across sizes beyond using the same binary.

## 8. Reproducibility

**Released with the paper:** the harness (`benchmarks/knowledge-offload`: `run.py`, `ko/`, the statistics scripts `stats_e0.py`, `stats_e1.py`, `stats_holm.py`, `stats_h5h8.py`, `stats_variants.py`, `web_payload_audit.py`, `label_abstain.py`), `models.lock` (repo, file, size, sha256, quant, lineage for every model), the per-question result JSON for every committed cell (`results/popqa-stratified1u-*.json`; each records model, quant, commit, seed, date, endpoint), the 50 hand labels, and every table. A figure can be rebuilt from the JSON without a GPU. The test suite of the harness is hermetic (no model, no network); 141 tests pass as of the variants commit (notebook).

**Not released with the paper:** the raw web search payloads (`results/payloads/`, gitignored and held on the benchmark machine only: they contain third-party page text, and the web numbers cannot be re-scored without them). `[TBD: decide what is released; either a scrubbed derivative or the claim "web numbers are not re-scorable from the release"]`. No data from the production memory pool is ever used or released.

**How a run is made:** one script per size, `scripts/ko_swap.py`, stops the production model, starts the named size with identical flags, checks the file's sha256 against `models.lock` before stopping production, runs the cells (resumable), and restores the production model in a `finally` block; a run refuses to start in a cron window and ends 17 minutes before the next one. **Swap windows** are recorded in `results/queue*.log` and `ko_swap-*.log`.

**What the paper's numbers depend on and was not independently verified (marked):**
- `[UNVERIFIED]` GPU **power cap of 119 W** and the thermal state during runs: the coordinator mentioned a 119 W cap in the instruction for this draft, but no committed file in the repository states it. Not reported as a fact until a log (`rocm-smi`) is committed. `[TBD: coordinator reads rocm-smi power cap and driver/ROCm/llama.cpp build versions on the benchmark machine and records them in the notebook; item 14]`.
- llama.cpp build hash, ROCm version, kernel: `[TBD: not recorded in any committed table; the ko_swap logs may contain the build line, to be read]`.
- Determinism: asserted from E0 (0 of 1,000, five conditions) and the E1 pilot (0 of 100 differ, three conditions, 9B IQ4_XS); not re-checked for every cell.

---

## Appendix A. Pre-written conclusions for the variants (written 2026-11-17, before any variant result exists)

*Purpose: so that the text cannot be bent to the result. Each branch is the text this paper will carry if the corresponding pre-registered reading of the notebook holds. The readings are R1–R5 as fixed in `NOTEBOOK.md` ("step 2 of the 9B plan"); they are reading aids with intervals, not tests, and outside Holm. When `VARIANTS_TABLE.md` exists, exactly one branch is kept; the rest are deleted and the deletion is logged in the notebook, naming the reading, the cells that decided it and the numbers. A result that fits none of them is reported as "none of the pre-registered readings" with the table, not forced into one.*

Notation: "baseline" is the default `tool` cell of the same file (9B IQ4 0.001, 4B IQ4 0.875, 4B Q8 0.574, 9B Q8 0.004). "Moves" means the lookup rate's 95% CI does not include the baseline rate. `[X]` is a number to be read from `VARIANTS_TABLE.md`.

**A.R1 — any variant takes the 9B IQ4_XS lookup to ≥ 0.5.**
> "The 9B's lookup skip is a property of one wording, not of the checkpoint: under `[variant]` it looked up `[X]` of questions (95% CI `[…]`), against 0.001 under the default prompt. The default prompt therefore is not one prompt but a family, and the lookup propensity of a model under 'the default prompt' is a range over wordings. We report H8 as a range over four wordings (the default and the three variants) and **make no statement about size**. The contract's effect on the 9B in §5.4 is the effect of replacing a fragile wording with a guarantee."
→ §5.3 H8 paragraph is rewritten as a range; §5.4 "what it means" gains the second sentence; the abstract's H8 sentence changes.

**A.R2 — 9B ≤ 0.05 under all three variants, and 4B IQ4_XS ≥ 0.5 under all three.**
> "The difference between the 4B and the 9B checkpoints in lookup behaviour survives three rewordings of the default prompt: the 9B looked up at most `[X]` under every variant, the 4B at least `[Y]`. We read this as a difference between these two checkpoints under this family of prompts. It is not a statement about size (the 27B behaves like the 4B, the 0.8B and 2B like the 9B), and it is not a statement about all prompts: `search_first` makes the 9B look up 0.898."
→ H8 stays non-monotone and gains "robust to three wordings".

**A.R3 — the 4B IQ4_XS lookup falls below 0.5 under some variant.**
> "The 4B's willingness to look up is also wording-dependent: under `[variant]` it fell from 0.875 to `[X]`. The statement 'the 4B and the 27B look up and the 9B does not' holds for one wording only."
→ the §5.3 sentence about the 4B and 27B is weakened; stage C (27B) is triggered for the same wording, per the pre-registered rule; §5.3's table gains the 27B variants if run.

**A.R4 — only `tool_free` moves lookup.**
> "The sentence 'reply with only the answer … no explanation' is what suppresses the lookup. Removing it moved the 9B IQ4_XS to `[X]`; reordering the sentences and describing the tool did not (`[Y]`, `[Z]`). `search_first` presumably works because it replaces that sentence with an instruction, `[UNVERIFIED: not tested]`. Answers under `tool_free` are longer; `contains` is used throughout, and `[N]` of `[M]` answers reached the token limit."
→ the mechanism sentence is allowed in §5.3; the paper still does not claim why the sentence suppresses calls.

**A.R5 — nothing moves the 9B, but `search_first` does.**
> "The 9B needs an explicit instruction to look up first: description (`tool_describe`), order (`tool_first`) and removing the format sentence (`tool_free`) leave the lookup rate at `[X]`, `[Y]`, `[Z]`, while 'look it up first' gives 0.898. The model does not decide for itself that it should look up in this setup; which is also what the contract (§5.4) supplies from the harness side."
→ §5.4's "what it means" is strengthened with this sentence; the discussion points to the contract as the portable fix.

**A.Q (stage B, quantisation; written here, not in the notebook's R1–R5).** *These two readings are proposed in this draft on 2026-11-17 and are not part of the frozen notebook entry; they must be copied into the notebook before the table is read, or dropped.*
- **A.Q1 — the IQ4 versus Q8_0 gap at 4B (0.875 vs 0.574) persists under each variant (its 95% CIs do not overlap at the same variant).** "The effect of quantisation on lookup propensity in the 4B is not specific to one wording: …"
- **A.Q2 — the gap closes under some variant.** "The quantisation effect on the 4B's lookup rate depends on the wording: under `[variant]` the IQ4_XS and Q8_0 rates are `[X]` and `[Y]`. The quantisation sentence of §5.3 is limited to the default prompt."

**A.None — the table fits none of R1–R5 (for example, 9B between 0.05 and 0.5 under some variants, or 4B moves under only one).**
> "Under three rewordings the 9B's lookup rate was `[X]`, `[Y]`, `[Z]` and the 4B's `[…]`; none of the pre-registered readings applies. We report the rates and make no inference about mechanism."
→ H8 stays as in §5.3; the abstract carries the measured range only.

**What none of the branches may say** (fixed in the notebook): any mechanism beyond the wording tested; any other wording, family or size not run; and anything about knowledge-removed models.

---

*Count after §4.7 (commit of 2026-11-17): 30 `[TBD` tags (one is the format example in the header), 5 `[OPEN`, 6 `[UNVERIFIED` (these include mentions in the status tables); recount with `grep -o '\[TBD' docs/knowledge-offload-paper-draft.md | wc -l`. First commit had 24 / 3 / 4.*
