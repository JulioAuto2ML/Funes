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
| Abstract | | **drafted with placeholders** (E1, variants and H2 numbers in; E2/E3, H7 open) |
| 1 | Introduction | drafted |
| 2 | Related work | drafted from abstract-level reading only; **full read of each paper pending** (see 0.4) |
| 3 | Setup: models, data, conditions | drafted in full |
| 4 | Methods: metrics, statistics, validity rules | drafted in full |
| 5.1 | E1 headline: facts flatten the size curve (H1) | **drafted with measured numbers** |
| 5.2 | What the store costs: reading capacity and retrieval | drafted with measured numbers |
| 5.3 | When the model looks up on its own (H8, variants) | drafted with the stage A/B variants (12 of 12 cells, 2026-11-18); R1 and R4 hold, R2, R3, R5 do not |
| 5.4 | Forcing the lookup: the contract (H5) | drafted with measured numbers and the negative result |
| 5.5 | Web as the store | drafted; 27B cell partial; **blocked on a search budget** for more cells |
| 5.6 | Write-back (H6, exploratory) | drafted (negative) |
| 5.7 | Fact-free reasoning (H2) | drafted with the GSM8K arm (23 of 23 cells, 2026-11-20); one `[TBD]` (other suite), one `[UNVERIFIED]` |
| 5.8 | Conflicts and edits (H3); knowing when to look up (E3) | placeholder, **blocked on E2/E3 (not run)** |
| 5.9 | Does it fit a smaller model? (H7, E7) | placeholder, **blocked on the Funes skill suite (not built)** |
| 4.7 | How the work was organised (sessions, rules, channels, failures, limits) | **drafted from the branch; one `[TBD]`** (the coordinator's effort level is not recorded) |
| 5.10 | Models with knowledge removed (E1b/E5/E6, H4) | `[OPEN: depends on the route and its results]`; prerequisite measurements written (2026-11-20: environment, GPU run, lookups, training throughput, estimated hours); **no claim about the models** |
| 5.11 | Teaching the lookup with a LoRA (E5, route C) | **drafted with measured numbers (2026-11-21)**: H9a and H9b hold, H9c does not; one seed; three `[TBD]` |
| 6 | Cost table (VRAM, latency, API cost, taint) | partly measurable now (wall time); VRAM/taint **not recorded** by the harness |
| 7 | Limitations | drafted, written early on purpose |
| 8 | Reproducibility | drafted in full |
| App. A | Pre-written conclusions for the variants, one per reading | two branches kept (A.R1, A.R4), the rest deleted and logged |

### 0.2 Placeholder count
23 real `[TBD]`, 3 `[OPEN]`, 4 `[UNVERIFIED]` at the first commit; recount with grep, see the last line.

### 0.3 What must be measured before submission, and who

| # | measure | blocks | who | cost (measured / not measured) |
|---|---|---|---|---|
| 1 | Variants stages A+B (12 cells) → `VARIANTS_TABLE.md` | §5.3, App. A | **done 2026-11-18**: queue finished 01:22 (coordinator's entry), 12 of 12 cells, `verify_variants.py` ok; Opus read | queue upper bound 4.9 h (notebook, Sonnet entry). **One assumption of that estimate already broke:** `tool_free` at 9B IQ4_XS took 1,160.4 s, above the 973.9 s `search_first` cell used as the high end of the per-cell range (coordinator's read-only measurement, 2026-11-17; not analysed here). Any cost range cited in the paper must say so; real total time: queue ended 01:22 (3.5 h before its guard); per-block times are in `results/queue_variants.log`, `[TBD: sum them; not read here]` |
| 2 | Stage C (27B) / D (2B) only if rule R3 or R5 fires or julio asks | §5.3 | coordinator | 27B 2.3–2.6 h extrapolated from `wall_s` |
| 3 | GSM8K arm (`closed`, `store`/`tool`, 5 sizes) | H2, §5.7 | Sonnet (code, stats), coordinator (GPU) | **done 2026-11-20**: 23 of 23 cells verified (`verify_gsm8k.py --with-tool`), `GSM8K_TABLE.md`; §5.7 |
| 4 | `funes` rows, one per size (D1) | the "deployed agent" claim, taint cost | coordinator; Sonnet for the stats | about 6 h (notebook, "julio: decide…" line; E0 measured 85 min per repeat at 9B) |
| 5 | A request-level capture of the tools block as served, or accept the GGUF template hash as sufficient | §5.3 mechanism wording | coordinator (needs a loaded model) | **not measured** |
| 6 | E2 (conflicts) and E3 (when to look up) | H3, §5.8 | Sonnet (code), coordinator (GPU) | **not measured** |
| 7 | Funes skill suite and size sweep with the store | H7, §5.9 | Opus designs, Sonnet codes | **not measured**; the suite does not exist |
| 8 | Run-to-run noise outside greedy: a second server/hardware, or temperature > 0 repeats | §7 limitation | coordinator | **not measured** |
| 9 | A second model family at 2–3 sizes | §7 limitation, H8 | julio decides | **not measured** |
| 10 | 27B `web` full cell and `web+recall-empty` control | §5.5, §5.6 | **needs a written search budget approved by julio** (Parallel credit is spent) | cost per search **not measured**; the 27B cell needs ≈ 2,100 searches (measured searches per searched question 2.2) |
| 11 | 20,000-resample H1 interval and a bootstrap that resamples sizes' dependence | §5.1 | Sonnet | no GPU |
| 12 | Knowledge-removed models (E1b/E6/E5): ROCm torch check, reproduction hours | §5.10 | Sonnet (script), coordinator (read-only yoda + GPU window), Opus + julio choose the route | **partly measured 2026-11-20** (§5.10.1): torch 2.12.1+rocm7.2 runs the 382M LMLM; 3.23 GB VRAM; training 5,353–5,675 tokens/s on one GPU; hours **estimated** 32–45 days. Not measured: full database without cap, FAISS index, any evaluation, a matching batch size. The route is not chosen |
| 13 | Hand-label check of the abstain regex with more than 50 answers and a second labeller | §4.4 | julio | 50 labels exist; second labeller does not |
| 14 | Verify the power cap, thermal state and driver versions reported in §8 | §8 | coordinator | `[UNVERIFIED]`, see §8 |

### 0.4 Citations
Only papers on the plan's alphaXiv-verified reading list (2026-11-14) are cited. On 2026-11-17 this session re-found these on alphaXiv by title and read their abstract line: 2505.15962, 2607.07707, 2607.00605, 2606.26807, 2404.11672, 2605.29075, 2603.17781, 2610.25425. The others on the list (2607.12831, 2411.13504, 2212.10511, 2202.05262, 2210.07229, 2608.14290, 2510.02091, 2510.03366, 2503.23084, 2609.35312) are cited by title and arXiv id from the plan only, with no characterisation beyond the title; **before submission each needs a full read** `[TBD: Sonnet or Opus reads each paper's text via alphaXiv and rewrites §2]`. Yang, Gao & Wu 2507.18178 is *not cited* (the plan says it is not verified).

---

## Abstract

Small language models answer long-tail factual questions badly, and bigger models answer them better mostly because they store more facts. We ask how much of a model is left for everything else if the facts live outside it. We run one model family (Qwen3.5, 0.8, 2, 4, 9 and 27B parameters, 4-bit quantised, with 8-bit controls up to 9B) on 1,000 unambiguous PopQA questions in a fixed harness, with the facts in the weights (`closed`), in the prompt (`oracle`, `store`), reachable by a tool the model may call, or on the web. Giving the model its facts cuts the accuracy-versus-size slope to 0.27 of the closed-book slope (95% CI 0.21 to 0.35), and the 0.8B model with a local store (0.921) beats the 27B model closed-book (0.431) by 0.490 points (CI 0.457 to 0.523). The tool route is harder: whether a model calls the lookup tool when the prompt only says "use the tools when they help" does not follow size (lookup rate at 0.8, 2, 4, 9, 27B: 0.000, 0.000, 0.875, 0.001, 0.887), and a harness contract that forces one lookup is what makes the small models usable (9B: 0.347 without, 0.991 with). We could not show that forcing the lookup costs no accuracy where the model already looks up (equivalence within two points not shown, pooled p = 0.135). The 9B's skip is wording-dependent: removing the sentence that asks for a short answer takes its lookup rate from 0.001 to 0.732 (95% CI 0.704 to 0.759), while reordering the two sentences or describing the tool leaves it at 0.000 (one greedy run per cell, descriptive). On a reasoning task that needs no facts (500 GSM8K problems, same five sizes) the same store leaves the size curve intact (slope ratio 1.04, 95% CI 0.959 to 1.124), as a negative control should; irrelevant retrieved facts cost one to four points at every size. `[TBD: H3/E2 and E3 results]`. `[TBD: H7: whether a model within 8 GB with the store is within five points of the 27B on a skill suite]`. `[OPEN: sentence on knowledge-removed models, only if the route is run and reported]`. The negative and exploratory results are reported as such.

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
H1–H8 were written in the plan and committed before the E1 sweep (plan, "Hypotheses, stated before running"; reworded where noted by date in the plan and by decision numbers D1–D16 in the notebook). The confirmatory family is H1, H2, H3, H4, H5 and H7 (m = 6); H6 and H8 are exploratory and outside it. H3, H4 and H7 have no data, and enter Holm as p = 1 (H2 has data since 2026-11-20); so every adjusted p-value in this paper is a **conservative** bound and will be recomputed when those tests exist. Equivalence tests (H5a) use TOST with a ±2 point margin; equivalence is declared only when the TOST p is below the Holm threshold.

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
| coordinator (julio's main thread) | model-driven session Sonnet 5.5 (as reported by the coordinator on 2026-11-17, message to Opus; not recorded in a committed file) · medium (effort stated by julio on 2026-11-17, in chat; not recorded in a committed file) | assigns tasks, starts and stops sessions, launches GPU jobs in a window julio chooses, writes briefs, relays messages (e.g. the template read, the variants queue decision, entries headed "coordinator") | doing another role's task silently |
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
- **The notebook**: a "Now" block (state, what is not run, what is invalid, pending decisions) and dated entries, each with a header *date · step · topic · role · model · effort*; 28 entries to 2026-11-17. Decisions carry identifiers **D1–D16** (D1–D4 Fable, D5–D8 and D9/D10 Opus, D11–D16 Opus after E1; D9 was superseded by D10 the same day and says so; the last decision on the branch is D16, confirmed by the coordinator). Code sessions end their entry with "Open for Opus"; the design role answers those items in its own entry (audit entry, "Answers to Sonnet's 'Open for Opus'"), which is how a question crossed from one session to another without a shared context.
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
6. **A GPU test took production down.** The first LMLM test (2026-11-20 11:38) was started with the 27B still serving; about 40 s in the kernel's out-of-memory killer ended `funes`, both WhatsApp bridges and the monitor, and the machine stopped answering until julio rebooted it at 14:49. The cause was measured afterwards (the card's 17.1 GB was full, `vram.log`); the free VRAM before the run had been measured as about 2.1 GB and the test needs about 2.6 GB. After the reboot a PID-file-based watchdog did not restart the language-model servers, and one WhatsApp bridge needed re-pairing. The run produced no result; the second run, with julio's explicit OK to stop the 27B, did. Cost: about 3 hours of production unavailable (11:39 to 14:49) `[UNVERIFIED: time to full recovery]`. The notebook entry says julio had said "ya podemos correr" a day before the window he had set; it does not say whether the 27B was to be stopped first.
Total: three errors that touched published numbers or GPU hours (items 1–3), one set of four stale or contradicted statements (item 4), four disclosed slips (item 5, a–d), and one production outage caused by a GPU test (item 6). Only the lineage error has a measured cost in GPU hours (about 6); the outage's duration is measured from the journal and the reboot time, the cost of the others was not measured.

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
- **Holm.** H1 raw p ≤ 1e-4 (the bootstrap's resolution); Holm-adjusted (m = 6, three tests with p = 1; H2 enters with its GSM8K p, §5.7) 0.0006 (`results/HOLM_TABLE.md`, sha256 `528b993e9be2e334afd6ba437d904e507c30ceaf6fa9b3f7492e80238e48fd90`, as of e4432cd; the earlier a731f5ea… table had H2 as p = 1). **H1 on Q8_0 is not testable in full** (no 27B Q8_0 cell), so its H1b half is p = 1 there.

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

**Pre-registered variants (stages A and B).** To test whether the 9B's skip is a property of the checkpoint or of one wording, three variants of the default prompt were fixed before any run (`NOTEBOOK.md`, entry "step 2 of the 9B plan", approved by julio and frozen): `tool_first` (the two sentences in the other order), `tool_free` (the sentence asking for a short answer removed), `tool_describe` (the tool described, not ordered). Stage A: 9B and 4B IQ4_XS. Stage B: 4B and 9B Q8_0. Stages C (27B) and D (2B) were not run. Each cell is 1,000 questions, greedy, one repeat; intervals are question bootstraps; there are no p-values and the variants are outside the Holm family.

*Source: `results/VARIANTS_TABLE.md` (sha256 `20aa71c57cc848d707dd984891d74d5f875a23b423198c3a8a03a2746f49185d`), 12 of 12 cells, `verify_variants.py` exit 0 (notebook, 2026-11-18).*

| lookup rate [95% CI] | default (baseline) | `tool_first` | `tool_free` | `tool_describe` |
|---|---|---|---|---|
| 9B IQ4_XS | 0.001 | 0.000 [0.000, 0.000] | **0.732** [0.704, 0.759] | 0.000 [0.000, 0.000] |
| 9B Q8_0 | 0.004 | 0.044 [0.032, 0.057] | **0.763** [0.737, 0.789] | 0.001 [0.000, 0.003] |
| 4B IQ4_XS | 0.875 | 0.811 [0.787, 0.835] | 0.911 [0.893, 0.929] | 0.977 [0.967, 0.986] |
| 4B Q8_0 | 0.574 | 0.480 [0.449, 0.510] | 0.762 [0.735, 0.788] | 0.602 [0.572, 0.632] |

| `contains` accuracy [95% CI] | default | `tool_first` | `tool_free` | `tool_describe` |
|---|---|---|---|---|
| 9B IQ4_XS | 0.347 | 0.335 [0.306, 0.364] | 0.933 [0.917, 0.948] | 0.345 [0.316, 0.374] |
| 9B Q8_0 | 0.350 | 0.377 [0.347, 0.407] | 0.913 [0.895, 0.930] | 0.342 [0.313, 0.371] |
| 4B IQ4_XS | 0.975 | 0.945 [0.931, 0.959] | 0.985 [0.977, 0.992] | 0.984 [0.976, 0.991] |
| 4B Q8_0 | 0.791 | 0.719 [0.691, 0.747] | 0.924 [0.907, 0.940] | 0.826 [0.802, 0.849] |

(Baselines are the committed default cells, `H5H8_TABLE.md`, not re-run; the accuracy baselines are those quoted in the coordinator's notebook entry of 2026-11-17.)

**The pre-registered readings, applied as written (notebook, "step 2 of the 9B plan", copied at the foot of `VARIANTS_TABLE.md`).**

| reading | condition as fixed | holds? | on which cells |
|---|---|---|---|
| R1 | a variant takes 9B IQ4 lookup to ≥ 0.5 | **holds** | `tool_free` 0.732 (CI lower bound 0.704) |
| R2 | 9B ≤ 0.05 under all three **and** 4B IQ4 ≥ 0.5 under all three | **does not hold** | the 4B part holds (min 0.811); the 9B part fails: `tool_free` 0.732 |
| R3 | 4B IQ4 falls below 0.5 under some variant | **does not hold** | 4B IQ4 minimum is 0.811 (`tool_first`) |
| R4 | `tool_free` moves lookup and the others do not | **holds**, read on 9B IQ4 | `tool_free` 0.732; `tool_first` 0.000, `tool_describe` 0.000, baseline 0.001 |
| R5 | nothing moves the 9B but `search_first` does | **does not hold** | `tool_free` moves it |

Two judgments, stated so they can be disputed. (i) **R3 is read as written: 4B IQ4_XS.** The 4B Q8_0 cell under `tool_first` is 0.480 (CI [0.449, 0.510]), below 0.5, but that is a different checkpoint (Q8_0, baseline 0.574); R3 names IQ4_XS and was fixed with that scope, so it is not extended. The Q8_0 number is reported in the table, not turned into a reading. (ii) **R1 and R4 hold together** (both say the 9B's skip depends on wording; R4 adds that, among the three, only `tool_free` moves it). R4 is read on 9B IQ4_XS, the cell the variants were designed for. Read cell by cell, "moves" (the CI excludes the baseline) is also true for 9B Q8_0 `tool_first` (0.004 → 0.044) and for the 4B `tool_first` and `tool_describe`; those are small or in the other direction (the 4B IQ4_XS falls to 0.811 under `tool_first`, rises to 0.977 under `tool_describe`), and none reverses the 4B's behaviour.

**What the table says, and no more.**
- The 9B's skip is not one fixed property of the checkpoint: with the format sentence removed it looks up on 0.732 (IQ4_XS) and 0.763 (Q8_0) of the questions. "The default prompt" is therefore a wording, and H8 is reported as a range over four wordings of the 9B (0.000 to 0.732 at IQ4_XS). We make **no statement about size**.
- Of the three variants, only the one that removes the format sentence moves the 9B; ordering the sentences differently and describing the tool do not (IQ4_XS 0.000 and 0.000; Q8_0 0.044 and 0.001). This is a pattern among the three wordings run, in two quantisations of one model. We do not claim why: one intervention (deleting a sentence) is the only one that works, and we did not test other deletions or other wordings.
- `search_first` still looks up more on the 9B IQ4_XS (0.898, `E1_TABLE.md`) than any of the three variants (point estimates; the intervals were not compared).
- In all eight 4B cells (four wordings, two quantisations) the lookup rate is 0.480 or more, and at IQ4_XS 0.811 or more. The 4B's quantisation gap holds under every wording shown (IQ4_XS above Q8_0: 0.875 vs 0.574, 0.811 vs 0.480, 0.911 vs 0.762, 0.977 vs 0.602; the CIs do not overlap at any of the four). This is a **post-hoc description**: the draft of 2026-11-17 proposed two quantisation readings (A.Q1/A.Q2) that were never copied into the notebook before the data, and so they are dropped as readings (App. A).
- **`tool_free` accuracy is not comparable with the other cells'.** Without the short-answer sentence the answers are sentences: mean 50.6 completion tokens against 3.9 (9B IQ4_XS), and in the 268 answers without a lookup the mean is 25.3 words, exact-match is 0 and `contains` is 204 (0.761). `contains` rewards a longer answer that mentions the gold string, and we did not measure how often that happens by chance. In those 268, 142 fall in popularity bucket 1e5 (the most popular; 136 of the 142 contain the gold string), against 194 questions in that bucket overall: the model skips what it is likely to know, as in the 4B and the 27B above, but the 0.761 is not an estimate of what the 9B knows closed-book (0.350). What the cell does support is the lookup rate (0.732) and, among the 732 looked-up answers, 729 contain the gold string (0.996; exact 0 here too).
- Share of answers at the 128-token proxy limit is 0.000 to 0.012 in every cell (`VARIANTS_TABLE.md`), so truncation does not drive these rates; the proxy sums the completion tokens of all calls in an answer and so is imprecise.

**Not shown.** Any mechanism; any wording other than these three and `search_first`; any family other than Qwen3.5; sizes 0.8B, 2B and 27B under the variants (not run); anything about models with knowledge removed.

### 5.4 Forcing the lookup: the contract (H5)

*Sources: `H5H8_TABLE.md`; `HOLM_TABLE.md`.*

- **Accuracy (answerable questions).** Without the contract, the default `tool` accuracy is 0.191 / 0.245 / 0.975 / 0.347 / 0.979 at 0.8 / 2 / 4 / 9 / 27B (IQ4_XS), which is the lookup rate's shadow; with the contract it is 0.190 / 0.374 / 0.991 / 0.991 / 0.996. The contract makes the 9B go from 0.347 to 0.991 (+0.644 [0.615, 0.674], 644 vs 0 discordant). At 0.8B it does nothing, because the model never calls the tool even when nudged (lookup rate 0.000). At 2B IQ4_XS it works for a fifth (0.212) of the questions only; at 2B Q8_0 it works for 0.958.
- **H5a (equivalence within ±2 points where the model already looks up): not shown.** Eligible sizes are those where the contract's lookup rate is ≥ 0.99 and the plain tool's is ≥ 0.8 (D12): IQ4_XS 4B (+0.016 [+0.009, +0.024], 16 vs 0 discordant, TOST p = 0.185) and 27B (+0.017 [+0.009, +0.025], 17 vs 0, TOST p = 0.26). Pooled IQ4_XS: +0.017 [+0.011, +0.022], TOST p = 0.135; Holm-adjusted 0.541 (0.676 before H2 had data). For Q8_0 **there is no eligible size**, so H5a cannot be computed there. In words: where the tool is already used, forcing it gained about 1.7 points, in the same direction at both sizes, and our data neither show equivalence within two points nor a difference we could assert after correction. Equivalence at this margin needs more power than 1,000 questions at these accuracies give.
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

*Sources: `results/GSM8K_TABLE.md` (sha256 `dcb45572ba1d23cc75cffbf1e14a0a0e2806f38c6561d38a4482e1689556dd3c`) and `results/HOLM_TABLE.md` (sha256 `528b993e9be2e334afd6ba437d904e507c30ceaf6fa9b3f7492e80238e48fd90`), as regenerated in commit e4432cd; design in `NOTEBOOK.md`, "H2 / GSM8K arm: design for implementation" (2026-11-18, written before any cell ran); run and reading in the entries of 2026-11-20. Cells `results/gsm8k-n500-s1-*.json`, 23 of 23 verified by `verify_gsm8k.py --with-tool`.*

**Design (frozen before the run).** 500 GSM8K test problems (1,319 in the file, sha256 `3730d312f6e3440559ace48831e51066acaca737f6eabec99bccb9e4b3c39d14`; `random.Random(1)`; the same 500 in every cell), greedy, one repeat. Two conditions with one system prompt: `closed` (the problem only) and `store` (the PopQA `store` condition unchanged: five facts from the 14,267-triple PopQA store, none of which a word problem needs, then the problem). The reply is visible step-by-step reasoning (reasoning budget 0, so no hidden channel), `max_tokens` 512. Primary metric: the last `#### N` line, else the last number in the reply, numerically equal to gold; a reply with no number is wrong. A strict-only accuracy (no fallback) is reported beside it. Pilot (20 problems outside the sample, `closed`): 0 of 20 at the limit on the 9B IQ4_XS, so 512 stayed (notebook, 2026-11-19). The IQ4_XS curve is 0.8, 2, 4, 9, 27B; the Q8_0 curve 0.8, 2, 4, 9B.

| `contains`-equivalent accuracy (`correct`), 500 problems | 0.8B | 2B | 4B | 9B | 27B |
|---|---|---|---|---|---|
| IQ4_XS `closed` | 0.436 | 0.726 | 0.910 | 0.934 | 0.952 |
| IQ4_XS `store` | 0.412 | 0.690 | 0.884 | 0.922 | 0.940 |
| Q8_0 `closed` | 0.534 | 0.710 | 0.922 | 0.932 | – |
| Q8_0 `store` | 0.506 | 0.682 | 0.898 | 0.924 | – |

(95% intervals per cell are in `GSM8K_TABLE.md`; they are about ±0.04 at 0.8B and ±0.02 at 27B.)

**H2, by the pre-registered rule: confirmed on both quantisations.** The slope of accuracy on log10(parameters in B) is 0.326 [0.299, 0.354] for `closed` and 0.339 [0.310, 0.367] for `store` on IQ4_XS; the ratio ρ = store/closed is **1.04 [0.959, 1.124]**. On Q8_0 the slopes are 0.406 [0.365, 0.449] and 0.424 [0.380, 0.469], ρ = **1.04 [0.937, 1.164]**. The rule was: confirmed if the CI lower bound of ρ is ≥ 0.5, refuted if the upper bound is < 0.5. Both lower bounds are above 0.5 and also contain 1, so the original wording "store slope ≈ closed slope" is also met. The raw one-sided p (H0: store slope − ½·closed slope ≤ 0) is at the bootstrap's resolution, ≤ 1e-4, per quantisation; Holm-adjusted (m = 6, `HOLM_TABLE.md`) 0.0006 on both.

**What this is and is not.** `store` here is `closed` plus five irrelevant facts, so a ratio near 1 is what one expects if the store does nothing for reasoning. **Our reading (not a pre-registered claim): H2 works as a negative control for H1.** It shows that the flattening of the size curve on PopQA (§5.1, ratio 0.27) is not something the harness would produce on any task: where the facts are not what limits the answer, the store does not flatten the curve. It is not a discovery about reasoning. In particular we do not claim that a store helps or does not help reasoning; the store here is irrelevant by construction.

- **Ceiling.** The 27B `closed` is 0.952, above the 0.95 flag in the design. Registered rule unchanged; exploratory sensitivity (`GSM8K_TABLE.md`): without the 27B, ρ = 1.03 [0.937, 1.123], p ≤ 1e-4, same verdict. Accuracy is 0.91–0.95 from the 4B up, so on this sample the size gain on reasoning is mostly below 4B (`closed` 4B → 27B: +0.042); the plan's expectation that "size still helps" holds mainly there. Q8_0 has no 27B cell.
- **Format drift (2B Q8_0 `closed`).** Strict 0.74, fallback 0.22, none 0.04; primary accuracy 0.710, strict-only 0.572 (flagged "fallback-heavy" in the table; 2B Q8_0 `store` is 0.18, under the 0.20 threshold). The fallback answers are right 69 of 108 against 286 of 371 for the strict ones, so the last-number rule recovers most of them at some cost in noise (notebook, 2026-11-20; computed from the cell files, not in the table). On Q8_0 with strict-only scoring ρ = 1.03 [0.93, 1.14]: same verdict (notebook, same entry, own bootstrap, not `stats_gsm8k.py`). `[UNVERIFIED: how many fallback answers took a number that was not the answer]`.
- **The store lowers accuracy slightly (post-hoc, descriptive).** `store` is below `closed` at all nine size × quantisation pairs, by 0.008 to 0.036 (per-size paired intervals in the table; several touch 0, the 4B Q8_0 one, [−0.046, −0.004], does not). Pooled over the nine pairs there are 359 problems solved only by `closed` and 260 only by `store`; the pairs are not independent (same 500 problems, both quantisations) and we give no p-value. Reading: irrelevant retrieved context costs a point or two at one repeat; the gains of §5.1 are net of that cost. It is not part of H2's rule.
- **`tool` stage (exploratory, post-hoc; the five IQ4_XS sizes were the code session's choice, not the design's).** Accuracy 0.504, 0.710, 0.902, 0.942, 0.950 for 0.8 to 27B. The lookup rate was read afterwards: 8 of 500 answers at 0.8B, 2 at 2B and none at 4B, 9B and 27B (notebook, 2026-11-20). So the 0.8B `tool` cell above its `closed` cell (0.504 vs 0.436) is not retrieval; it is `closed` under a different system prompt with a tool schema in context (mean completion tokens 241 vs 147; more answers with no number, 33 vs 15). The cause is not measured. No hypothesis is attached to this stage and it enters no test.
- **Not shown.** Reasoning tasks other than GSM8K (`[TBD: BBH or another suite, if wanted]`); other prompts (one frozen prompt, no variants as in §5.3); anything about a store that holds relevant facts for a reasoning task; n other than 500 (cost; each cell's standard error is at most 2.2 points); more than one repeat.

### 5.8 Conflicts and edits (H3) and knowing when to look up (E3)
`[TBD: E2: context-faithfulness and parametric override per size, logistic regression of override on log(params); supersede vs contradict in Funes]`. `[TBD: E3: lookup rate per kind of question (answerable, weights-only, unanswerable), abstention, accuracy when the store is wrong or noisy]`. What is measured today about abstention is only §5.4's ablated arm.

### 5.9 Does it fit a smaller model? (H7)
`[TBD: Funes skill suite (does not exist), size sweep with the store, H7: a model ≤ 8 GB with the store within five points of the 27B]`.

### 5.10 `[OPEN: depends on the route and its results]` Models with knowledge removed
This section covers the optional second track: whether pre-training or editing that removes facts from a model changes what its parameters do (LMLM and Co-LMLM against their standard twins; layer ablation; a lookup LoRA). **julio decided on 2026-11-17 that this line of work is wanted. The route, the order and the cost are not decided, and no experiment on these models has been run.** What exists is a set of prerequisite measurements on one released pair, `LMLM-llama2-382M` and `Standard-llama2-382M`, on the machine of §3.6. They show whether the track is feasible here and at what cost. **They say nothing about whether removing knowledge works, about LMLM's accuracy, or about any of our tasks.** When the route is chosen, this section is rewritten from its own pre-registration; H4 (non-fact perplexity) belongs here and enters the Holm family with its own p-value.

**5.10.1 What was measured (notebook, entries of 2026-11-18 and 2026-11-20; all single runs).**

| item | result | measured / estimated |
|---|---|---|
| LMLM's pinned `torch==2.4.0` on this machine | not installable: yoda has only Python 3.13.5, and the ROCm index lists no 2.4.0 build for it (only 2.6.0+rocm6.1 for cp313) | measured (`pip download`) |
| Environment used instead | torch 2.12.1+rocm7.2, transformers 4.49.0 (pin kept), faiss-cpu 1.16.0, numpy 2.5.5, marisa-trie 1.4.1 (pin 1.2.1 not tried); for training, `trl==0.15.2`, which the repo does not pin and which was chosen by date | measured (`freeze.txt`); **the torch version differs from the authors'** |
| `LMLM-llama2-382M`, repo example, GPU (RX 9060 XT) | rc 0; 58.3 s for 6 prompts (max 512 new tokens, up to 10 lookups); VRAM peak 3,228,696,576 B (592,822,272 B already in use before, so about +2.64 GB); max RSS 3,289,544 kB | measured, one run |
| `Standard-llama2-382M`, same example | CPU (the repo's code path): 185.4 s; max RSS 2,697,688 kB; VRAM unchanged | measured; **different device from the row above, so not a model comparison** |
| Does it fit beside the production 27B? | no: the 27B holds 14,981,218,304 B of the card's 17,095,983,104 B (about 2.1 GB free) and the run needs about 2.64 GB more. A first attempt with the 27B serving ended in the kernel's out-of-memory killer about 40 s in and the machine stopped answering until julio rebooted it (§4.7, item 6); that run produced no result | measured (journal, `vram.log`); any LMLM run needs production stopped |
| Do the lookups happen? | yes: each of the 6 prompts contains 9 `<|db_entity|>…<|db_return|>…<|db_end|>` calls. The database is `dwiki_bio17k` (biographies). The one entity checked against the database (Ko Itakura) returned a plausible entry; for entities not in it, with `fallback_policy top1_anyway` the database returned an unrelated entry with no sign of doubt (Mao: "Paper Tiger"; Fábio A L de Souza: "Master's degree in Jazz Studies") | measured on 6 prompts; **not a rate** |
| Full database `dwiki6.1M` (54.6M triples) | downloaded, 7,921,290,218 B (equals the Hugging Face tree size); `json.load` under a 12 GB address-space cap fails with `MemoryError` after 13.7 s at max RSS 7,745,484 kB | measured; shows it does not fit under that cap, **not** that it does not fit in the machine's 15.8 GB; loading without the cap, entry count and the FAISS index were **not tested** |
| Training throughput with the repo's own trainer (`lmlm.training.pretrain`, `pretrain.sh` arguments, tiny-llama2-382M, packing, sequence 1024, gradient checkpointing, bf16, 30 steps, one GPU, 200 MB head slice of the corpus) | batch 8: 5,353 tokens/s (45.9 s); batch 16: 5,675 tokens/s (86.6 s); batch 32: out of memory. VRAM peak 16.07 GB and 16.42 GB (includes 0.6 GB used by other processes) | measured; 30 steps with warm-up, first steps included |
| Hours to reproduce the pre-training | **about 45 days** (2.20e10 tokens = 28,022,521,773 B × 0.0983 tokens/byte × 8 epochs, at 5,675 tokens/s; 47.6 days at 5,353) or **about 32 days** (1.57e10 tokens, from the checkpoint name `…ep8_bsz256…/checkpoint-60000` = 60,000 × 256 × 1,024; 34.0 at 5,353) | **estimated, not measured.** The tokens-per-byte ratio comes from the first 2,001 documents only; the two token counts disagree by 40% and which one the released model used is not known; the original script used 2 GPUs at batch 128 each, which this card cannot reproduce; throughput of 30 steps is extrapolated to weeks |

**5.10.2 What this supports and what it does not.**
- It supports: the released 382M models load and generate on this card under a newer torch than the authors'; the lookup syntax is produced and answered by the database; running them requires stopping production; reproducing the pre-training from scratch on this one card would take weeks, not hours (an estimate with the caveats above).
- It does not support any statement about accuracy, about whether the parameters hold fewer facts, about Co-LMLM, about layer ablation or about a lookup LoRA: none was run. The repo itself states about 8 H100-days for the 382M; we did not check that figure `[UNVERIFIED]`.
- `[TBD: LMLM and Standard evaluated on a task of ours; the full database loaded without a cap and its FAISS index; the training throughput at a batch size that matches the authors'; whether a smaller reproduction (fewer tokens) is informative]`.
- The `top1_anyway` observation is a property of the database fallback setting on 6 prompts, not a finding about the method; whether the model can decline to use a wrong return is untested.

### 5.11 Teaching the lookup with a LoRA (E5, route C)

§5.3 and §5.4 showed that whether a small model looks up by itself depends on a prompt, and that the prompt that works at 9B and at 2B Q8_0 does not work at 2B IQ4_XS (21% looked up under the contract) or at 0.8B (0%). E5 asks whether a small LoRA can supply that behaviour without a prompt. Design, written before any training and dated 2026-11-20: `E5_DESIGN.md` (route C, chosen by julio from five options). **What E5 is not:** it changes behaviour, not parameters. It removes no knowledge, so it says nothing about freed capacity and tests nothing of H4 (§5.10 stays open). **It does not replace LMLM.**

**5.11.1 What was done.**
- **Models and arms.** Qwen3.5-2B IQ4_XS (primary) and 0.8B IQ4_XS (secondary, reported separately, no pooling). Per model: **base** (the existing cells), **L1** (lookup trajectories) and **L0**, the control: same questions, same tools block, same final answers, but the model answers directly with no call. L0 is what lets a gain be attributed to looking up rather than to the short-answer format or PopQA's templates. Adapters were trained on the bf16 weights and served on the same IQ4_XS GGUF the baselines used (`--lora`), so base and adapter rows differ only by the adapter.
- **Data, no model writes it.** PopQA's 14,267 triples; 1,000 lookup examples per model (999 written, one dropped ungrounded) plus 500 anchors: the base model's own greedy answers to GSM8K **train** questions, half rendered with the tools block and half without, truncated answers dropped. No eval id and no triple sharing a subject URI with an eval item is in training (asserted in code). Three relation units were held out of training by a draw fixed beforehand (`Random(5)`: director, father, screenwriter, 244 eval items). The 1e5 popularity bucket had only 153 pool items for 195 wanted, so the other buckets were topped up (no repeats; julio's decision). Loss is on assistant tokens only; **the final answer's value is not masked** (flagged deviation: copying from the result is the skill taught).
- **Hyper-parameters.** Three dev settings, frozen before the first run (A r16/α32/lr 2e-4; B r8/α16/lr 1e-4; C r32/α64/lr 2e-4); fixed: 2 epochs, 376 steps, batch 8, seed 0. On 300 dev items all three looked up 1.000; the rule frozen beforehand (tie → dev accuracy) selected B (accuracy 0.990 against 0.987, one item). The stopping rule (retrain at 3,000 + 1,500 if dev lookup < 0.80) did not fire.
- **Checks before the eval cells.** Real chat template against the server's `/apply-template` and token-level mask decode (gates 9a/9b) passed. For each of the four adapters, at eval time: scale 0 reproduces the base replies exactly and scale 1 differs. `verify_e5.py` passed 21 of 21 cells before any statistic was computed. Adapter and base hashes are in `models.lock`.
- **Cost (measured).** Training wall: 2B L1 1,425 s, L0 1,399 s; 0.8B L1 976 s, L0 896 s; peak 9.34, 8.99, 5.13, 4.79 GiB; seven trainings in all about 2.4 h of GPU. The eval window took 2 h 44 min. Production was stopped for each window.

**5.11.2 The pre-registered family (Holm m = 3, own family; the main family's m = 6 is unchanged).** Raw p-values come from 10,000 question bootstraps, seed 0, one-sided as in `stats_holm.py`. **The p-value definitions (H9a: the larger of the two parts' shares of resamples at or below 0.80; H9b: share of paired resamples at or below 0; H9c: TOST, the larger of the two one-sided shares) were written in `stats_e5.py` by the coordinator after the data existed and before any number was read; the design fixed the decision rules (below) but not these definitions.** I reviewed them and reran the script: its table is reproduced byte for byte.

| test | rule written in the design | result | raw p | Holm adj. p | verdict |
|---|---|---|---|---|---|
| H9a it looks up | 2B L1 default-prompt lookup ≥ 0.80 on all 1,000 **and** on the held-out relations | 0.992 [0.986, 0.997] on 1,000; 1.000 on the 244 held-out | 0.0001 (resampling floor) | 0.0003 | **holds** |
| H9b lookup, not format | acc(L1) − acc(L0) on `tool`: CI lower bound > 0 and difference ≥ +0.10 | +0.705 [+0.676, +0.733]; L0 looks up 0.000 and answers 0.285, L1 answers 0.990 | 0.0001 | 0.0003 | **holds** |
| H9c skills intact | GSM8K `closed`, L1 − base, 95% CI inside ±0.05 | −0.034 [−0.074, +0.006] (accuracy 0.726 → 0.692) | 0.24 | 0.24 | **does not hold** |

**5.11.3 Reading the three tests.**
- **H9a.** Under the default prompt, where the base looks up 0.000 times in 1,000 questions, the adapter looks up 992 times; on the three relations it never saw, 244 of 244. The held-out interval printed by the script is a degenerate [1.000, 1.000], which is not a claim of zero uncertainty; the exact one-sided 95% lower bound for 244 of 244 is about 0.985 (0.025^(1/244), computed by hand for this paragraph, not by the script). The two p-values sit at the bootstrap's floor 1/10,001. 0.8B (reported only, not in the family): lookup 0.995 on all 1,000 (accuracy 0.979) and 0.996 on the held-out.
- **H9b.** The size of the difference is not informative about learning: L0 never looks up, answers from the weights (0.285 against the base's 0.245 on `tool`), and L1 gets the answer from the store. H9b tests that the gain is not the format; it did, with a margin of +0.705 over the +0.10 bar. A model-side claim would be a mistake: **accuracy of about 0.99 under lookup is the store returning the gold fact** (the 9B under a prompt contract reached 0.991 as well, §5.4), not knowledge in the weights.
- **H9c does not hold, and it is not a demonstrated loss.** The interval includes zero (upper bound +0.006) but its lower bound (−0.074) is outside the margin, so equivalence is not shown. The 90% interval (−0.068, 0.000, computed by me with the script's bootstrap) is also outside −0.05, so the verdict does not depend on how the TOST level is read (the gate "95% CI inside ±0.05" corresponds to one-sided tests at 2.5%, stricter than the "adjusted p < 0.05" gate the script adds; both fail). The margin was **attainable**: the base-vs-base repeat (GSM8K `closed`, 2B IQ4_XS, 500 problems, same server) gave a discordance of 0.000 and **500 of 500 identical generated texts**, so greedy decoding is reproducible here and the 20.2% of problems on which L1 and base disagree (42 problems L1 gets right and base wrong, 59 the other way) come from the adapter, not from noise. The design's attainability condition (discordance ≤ about 0.325) was met, but it is a necessary condition for a true difference of zero only: at the observed discordance of 0.202 the interval half-width is about 0.039, so equivalence to ±0.05 could only have been declared if the point estimate lay within about ±0.011 (my calculation, normal approximation, not measured). One repeat, one server build; no claim for other builds or batch sizes.

**5.11.4 Reported only (not in the family, no p-values).**
- **The control moves too, post hoc and exploratory.** Same bootstrap, committed cells, computed by me after seeing the table, **not pre-registered**: at 2B, L0 − base = −0.016 [−0.052, +0.020] and L1 − L0 = −0.018 [−0.048, +0.012]; at 0.8B, L1 − base = −0.088 [−0.134, −0.044], L0 − base = −0.086 [−0.132, −0.042] and L1 − L0 = −0.002 [−0.032, +0.028] (accuracies 0.436 / 0.348 / 0.350). At 0.8B the whole drop is shared by both arms, so it is not specific to lookup; at 2B the L1 − L0 difference is not distinguishable from zero. Both arms share the same LoRA recipe and anchors, so these data say "this recipe lowers GSM8K `closed` somewhat", and cannot say why. No mechanism is claimed.
- **Closed book.** 2B 0.243 → 0.238 (L1 `closed`); 0.8B 0.213 → 0.182. The adapter did not make the model stop answering from its weights when no tool is offered.
- **Paraphrase set P (200 items, 2 hand-written templates per relation, hashed before training).** 2B: L1 0.965 (lookup 0.950) against L0 0.260; 0.8B: 0.980 against 0.185.
- **Other wordings** (L1 was trained on `SYSTEM_TOOL` only): `tool_free` 2B 0.983, 0.8B 0.963; `tool_describe` 0.993 and 0.973.
- **Knowing when not to look up: not taught and not achieved.** With the `abstain_default` prompt (n = 200) 2B L1 looks up 1.000, answers 0.195, abstains 0.265, hallucinates 0.540 (0.8B: 0.640). Abstention was deliberately not part of the training (single variable).
- **Quantisation transfer.** The same 2B adapter on the Q8_0 base: accuracy 0.995, lookup 1.000. The scale-0 gate was run on the adapter's own base, not on Q8_0.
- **By relation and by popularity.** At 2B every relation looks up at 0.946 or more (lowest: `capital`, 0.946 of 130); by popularity bucket the lowest is 1e5, 0.969 of 194. The rows of 1e6 and 1e7 have 8 and 2 items.

**5.11.5 What this supports and what it does not.**
- It supports: at 2B and 0.8B IQ4_XS a LoRA trained on 999 lookup examples, one seed, makes the model call `lookup` on 0.992 (2B) and 0.995 (0.8B) of the 1,000 questions under the default prompt, including on three relations absent from training, where the prompt contracts of §5.4 had left 2B IQ4_XS at 21% and 0.8B at 0%. The control shows the accuracy gain comes from lookup and not from the answer format.
- It does not support: that the skill cost is nil (H9c is not met at 2B and the 0.8B drop is shared with the control); any statement about knowledge removed or freed capacity (H4 untested; §5.10); that the model knows when not to look up; that the result generalises across relations of a different kind, templates not seen (set P is partial evidence), tools other than `lookup`, or model families.
- Limits: **one seed and one run per arm**, so no variance across training seeds exists; the eval is in-distribution for templates and popularity profile; `director` and `screenwriter` are close to relations that were trained (`producer`, `author`, `composer`), so **`father`** is the clean held-out relation (50 items, all looked up); training in bf16 and serving in IQ4_XS were not compared numerically; the Q8_0 row was not gated on Q8_0; the 0.8B set of cells and the use of `transfer_bases` are choices julio confirmed after the fact; per-question payloads exist on yoda only (64 MB, not committed). `[TBD: a second training seed]` `[TBD: a held-out relation of a different kind, or a different tool]` `[TBD: the cause of the GSM8K shift, if julio wants it known: it would need an ablation not run]`.

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
10. **Small LMLMs.** If §5.10 is run (so far only its prerequisites were measured), the released LMLM models have 176M–382M parameters and complete biographies rather than follow instructions; they bear on whether masking changes the parameters, not on whether Funes could run on one.
11. **Post-hoc elements.** The sample, the metric hierarchy (D5), H5's reframing (D12–D14) and H8's reframing were decided after E0 and, for H5/H8, after E1; each is dated in the notebook. The Holm family fixes m = 6 with three empty entries (H3, H4, H7) as p = 1 (four until the GSM8K arm).
12. **Hash evidence.** The GGUF files are tied to the repositories by their LFS sha256 and verified by the runner; the template hashes were read from the file headers. We did not check the llama-server build's tool-call parsing across sizes beyond using the same binary.
13. **E5 is one seed per arm and in distribution (§5.11).** H9c's verdict (not equivalent) rests on one training run per arm and one repeat of the base; the lookup rate is measured on PopQA's templates and popularity profile; the held-out relations are partly close to trained ones; the GSM8K shift appears in the control too and its cause was not tested. The p-value definitions of H9a–c were written after the data existed and before any number was read.

## 8. Reproducibility

**Released with the paper:** the harness (`benchmarks/knowledge-offload`: `run.py`, `ko/`, the statistics scripts `stats_e0.py`, `stats_e1.py`, `stats_holm.py`, `stats_h5h8.py`, `stats_variants.py`, `web_payload_audit.py`, `label_abstain.py`), `models.lock` (repo, file, size, sha256, quant, lineage for every model), the per-question result JSON for every committed cell (`results/popqa-stratified1u-*.json`; each records model, quant, commit, seed, date, endpoint), the 50 hand labels, and every table. A figure can be rebuilt from the JSON without a GPU. The test suite of the harness is hermetic (no model, no network); 141 tests pass as of the variants commit (notebook).

**Not released with the paper:** the raw web search payloads (`results/payloads/`, gitignored and held on the benchmark machine only: they contain third-party page text, and the web numbers cannot be re-scored without them). `[TBD: decide what is released; either a scrubbed derivative or the claim "web numbers are not re-scorable from the release"]`. No data from the production memory pool is ever used or released.

**How a run is made:** one script per size, `scripts/ko_swap.py`, stops the production model, starts the named size with identical flags, checks the file's sha256 against `models.lock` before stopping production, runs the cells (resumable), and restores the production model in a `finally` block; a run refuses to start in a cron window and ends 17 minutes before the next one. **Swap windows** are recorded in `results/queue*.log` and `ko_swap-*.log`.

**What the paper's numbers depend on and was not independently verified (marked):**
- `[UNVERIFIED]` GPU **power cap of 119 W** and the thermal state during runs: the coordinator mentioned a 119 W cap in the instruction for this draft, but no committed file in the repository states it. Not reported as a fact until a log (`rocm-smi`) is committed. `[TBD: coordinator reads rocm-smi power cap and driver/ROCm/llama.cpp build versions on the benchmark machine and records them in the notebook; item 14]`.
- llama.cpp build hash, ROCm version, kernel: `[TBD: not recorded in any committed table; the ko_swap logs may contain the build line, to be read]`.
- Determinism: asserted from E0 (0 of 1,000, five conditions) and the E1 pilot (0 of 100 differ, three conditions, 9B IQ4_XS); not re-checked for every cell.

---

## Appendix A. Conclusions pre-written for the variants (written 2026-11-17, before any variant result existed; kept and deleted on 2026-11-18)

*Purpose: so that the text cannot be bent to the result. The readings are R1–R5 as fixed in `NOTEBOOK.md` ("step 2 of the 9B plan"); reading aids with intervals, not tests, outside Holm. `VARIANTS_TABLE.md` (sha256 `20aa71c5…`) was then read; §5.3 applies each reading as written. The two branches whose condition holds (A.R1 and A.R4, which are compatible) are kept below with the numbers filled in; the other branches are deleted and each deletion is logged in `NOTEBOOK.md` (entry of 2026-11-18, Opus) with the cells that decided it.*

**A.R1 — a variant takes the 9B IQ4_XS lookup to ≥ 0.5. (Holds: `tool_free`, 0.732 [0.704, 0.759].)**
> "The 9B's lookup skip is a property of one wording, not of the checkpoint: under `tool_free` it looked up 0.732 of questions (95% CI 0.704 to 0.759), against 0.001 under the default prompt. The default prompt therefore is not one prompt but a family, and the lookup propensity of a model under 'the default prompt' is a range over wordings. We report H8 as a range over four wordings (the default and the three variants) and make no statement about size."
→ §5.3 reports the range; the abstract carries one sentence on it.

**A.R4 — only `tool_free` moves lookup (read on 9B IQ4_XS). (Holds: `tool_free` 0.732; `tool_first` 0.000; `tool_describe` 0.000; baseline 0.001.)**
> "Of three changes to the default prompt, only removing the short-answer sentence moved the 9B IQ4_XS (to 0.732); reordering the sentences and describing the tool did not (0.000, 0.000). The same pattern holds for the 9B Q8_0 (0.763; 0.044; 0.001). Whether the sentence suppresses the lookup by itself, or in some interaction with the rest of the prompt, was not tested; neither was whether `search_first` works because it replaces that sentence `[UNVERIFIED: not tested]`. Answers under `tool_free` are sentences (mean 50.6 completion tokens against 3.9), so `contains` accuracy under it is not comparable with the other cells (exact-match is 0 there); the share of answers at the 128-token proxy limit is 0.012 or less in every cell."
→ the paper does not state a mechanism. (The pre-registered reading R4 says "the format sentence is the mechanism"; the paper words it more weakly than the notebook, because the three variants contain one deletion and no test of why it works. See the notebook entry of 2026-11-18.)

**Deleted, logged in the notebook:** A.R2 (9B part fails), A.R3 (4B IQ4_XS minimum 0.811), A.R5 (`tool_free` moves the 9B), A.None (R1 and R4 apply), and the proposed readings A.Q1/A.Q2 (never in the frozen notebook entry; the quantisation comparison is reported in §5.3 as post-hoc description, with the cells shown).

**What none of the branches may say** (fixed in the notebook): any mechanism beyond the wording tested; any other wording, family or size not run; and anything about knowledge-removed models.

---

*Count after §5.11 (2026-11-21, grep of this file): 31 `[TBD` tags (one is the format example in the header), 5 `[OPEN`, 10 `[UNVERIFIED` (these include mentions in the status tables); recount with grep.*
