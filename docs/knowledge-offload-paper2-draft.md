# Changing the Model: Can a Small Model Be Taught to Look Facts Up, and How Much Model Does a Local Agent Need?

**DRAFT 1 · 2026-11-22 · paper 2 of 2 (changing the model). Split from `docs/knowledge-offload-paper-draft.md` (the source file, kept) after julio's decision of 2026-11-22. Cites paper 1 (`docs/knowledge-offload-paper1-draft.md`), which this draft calls **P1**.**
Author: Julio Rodriguez Martino (independent). Drafted by the Opus session of the knowledge-offload track. Plan: `docs/knowledge-offload-plan.md`. Notebook: `benchmarks/knowledge-offload/NOTEBOOK.md`.

**Golden rule of the lab, applied to this text:** nothing is assumed, everything is measured. Every number is copied from a committed table and cited with its file; what does not exist is a visible placeholder `[TBD]`, `[OPEN]` or `[UNVERIFIED]`. This paper has **one experiment with results (E5, a lookup LoRA, one training seed)**, **prerequisite measurements only** for models with knowledge removed, and **open placeholders** for layer ablation (E6) and the agent-in-a-memory-budget question (E7/H7). The title's second question is stated because it is the one this line of work is for; **it is not answered here.**

---

## 0. Status of this draft (remove before submission)

| § | section | status |
|---|---|---|
| Abstract, 1 | | drafted around E5 |
| 2 | Related work | abstract-level; full reads pending (0.3) |
| 3 | Setup (recap of P1, plus the adapters) | drafted |
| 4 | Methods, Holm | drafted |
| 5.1 | E5: a lookup LoRA | **drafted with measured numbers** (21 cells verified, `E5_TABLE.md`); one seed |
| 5.2 | Models with knowledge removed (E1b, H4) | **prerequisites measured only**; no claim about the models |
| 5.3 | Layer ablation (E6) | `[OPEN]` not designed in code |
| 5.4 | Does an agent fit a smaller model? (E7, H7) | `[OPEN]` the skill suite does not exist |
| 6–8 | Cost, limitations, reproducibility | drafted |

### 0.1 Decisions that are julio's
- **Paper 2's scope** (E5 plus whatever of E1b/E6/E7 is run) is his. The second training seed of E5 is **explicit future work** and is not run; whether to run it, change the recipe, or add a held-out relation of another kind is his call (§7).
- **Whether the knowledge-removed route (E1b) is run, and in what order,** is not decided; nothing here assumes it will be.

### 0.2 What must be measured before submission
| # | item | blocks | state |
|---|---|---|---|
| 1 | Second training seed of E5 (2B L1 and L0 and their cells) | every claim in §5.1; H9c and H9a depend on one run per arm | not run; the 2.4 h of training and 2 h 44 min of eval windows are measured costs of the first run (§5.1.1) |
| 2 | Why GSM8K `closed` falls under the recipe (also in L0) | §5.1.3 | not tested; would need a recipe ablation (fewer steps, other anchors, another rate) |
| 3 | A held-out relation of a different kind, or a different tool | §5.1.5 | not run |
| 4 | The scale-0 identity gate on the Q8_0 base itself | the transfer row | not run |
| 5 | E1b: LMLM and Standard evaluated on a task of ours; the full database loaded; a FAISS index; training throughput at the authors' batch | §5.2 | not run; prerequisites in §5.2 only |
| 6 | E6 (layer ablation) | §5.3 | not designed; not measurable without a probe |
| 7 | E7 and the Funes skill suite (H7) | §5.4 | the suite does not exist |
| 8 | Per-question E5 payloads (64 MB, yoda only) | re-scoring | not committed |
| 9 | Second labeller and more than 50 labels for the abstention regex (inherited from P1) | `abstain_default` reading | 50 labels exist |

### 0.3 Citations
As in P1 §0.3. KBLaM (arXiv:2410.10450) was read through alphaXiv on 2026-11-22. LMLM, Co-LMLM and the others on the plan's list are cited as in P1 (abstract-level, full reads pending). The LMLM repository's own figure of about 8 H100-days for the 382M model was not checked `[UNVERIFIED]`. No web search was used.

---

## Abstract

Paper 1 (P1) measured where a fact should live for a model *as released*. Here we ask whether the model itself can be changed. We report one experiment with results and say plainly what is missing. **E5:** a small LoRA is trained so that Qwen3.5-2B and 0.8B (IQ4_XS) call a `lookup` tool on their own, under the default prompt, where without it they never do (lookup rate 0.000 at both sizes on 1,000 PopQA questions). One training seed per arm, with a control arm trained on the same questions and answers but no call (L0). **H9a holds:** the adapter looks up on 0.992 of the 1,000 questions at 2B (95% CI 0.986 to 0.997) and on 244 of 244 questions of three relations that were never in training. **H9b holds:** accuracy under lookup exceeds the control's by +0.705 (CI +0.676 to +0.733), so the gain is the lookup and not the answer format; accuracy near 0.99 is the store returning the gold fact, not knowledge in the weights. **H9c does not hold:** on GSM8K closed-book the adapter is 0.034 below the base (CI −0.074 to +0.006), not shown equivalent within ±0.05; the control arm falls by a similar amount, so the shift belongs to the training recipe, whose cause we did not test. Abstention was not taught and does not appear (0.540 hallucinated on absent facts at 2B). The Holm family of this paper (H9a, H9b, H9c) was written after P1 split off; H4 and H7 of the pre-registered six are untested and are reported as such. For models whose pre-training removed facts we report only what it takes to run them on our machine (a 382M model fits in 3.23 GB of VRAM; reproducing its pre-training on one card would take an estimated 32 to 45 days), and nothing about whether removal works. Layer ablation and the question of how much model a local agent needs are open. One seed, one model family and in-distribution questions limit every result.

---

## 1. Introduction

P1 showed that, for released models, putting the facts in a store flattens the size curve, that whether a model *uses* a store depends on a prompt more than on its size, and that a harness contract can force the use at a cost we could not bound. Its last open point is the reason for this paper: a prompt did not make the small quantised models look up (2B IQ4_XS looked up 21% of the time under the contract, 0.8B never), and the plan's real question, **how much model a local agent needs once the facts are elsewhere,** needs a model that already behaves that way. There are three ways to change the model: teach it to look up (a LoRA, E5), build it so that its parameters hold fewer facts (LMLM-style pre-training, E1b/H4), or remove capacity and see what breaks (layer ablation, E6). Only the first was run.

**Contributions.**
1. A pre-registered test (H9a–c, §4) of whether a LoRA trained on 999 lookup examples makes a 2B and a 0.8B model look up under the unchanged default prompt, with a control that has the same data and no call, and with three relations held out of training (§5.1). Two hypotheses hold and one does not; we report the negative as it is.
2. The negative-result structure: skills (GSM8K) fall with the recipe, in the control too; the adapter did not learn when *not* to look up.
3. Prerequisite measurements for running knowledge-removed models on this machine, with their cost (§5.2), clearly separated from any claim about those models.
4. A protocol for the process (inherited from P1 §4.7) and every committed result, so the E5 tables can be rebuilt without a GPU (§8).

What this paper does *not* claim: that fewer facts live in the weights after E5 (a LoRA changes behaviour, not parameters), anything about LMLM's accuracy, anything about layer ablation, that H7 holds or fails, or that the result carries to other families, other tools or other sizes.

## 2. Related work

*(Status: abstract-level reading only; to be rewritten after full reads, P1 §0.3.)*

**Models that externalise knowledge during pre-training.** Limited Memory Language Models (LMLM, arXiv:2505.15962) pre-train with factual knowledge moved into an external database that the model queries; Co-LMLM (arXiv:2607.07707) externalises knowledge with continuous queries and reports the goal of higher performance at smaller scales; a related audit studies forgetting in LMLMs (arXiv:2607.00605). Knowledge Offloading (KOFF, arXiv:2605.29075) decomposes a model into a sparse backbone and memory modules. Knowledgeless Language Models (arXiv:2607.12831), KARLA (arXiv:2606.26807), Mixture-of-Memories (arXiv:2610.25425) and Intern-S2-Mobius (arXiv:2608.14290) are recent members of this line. These works change the model from the start of training. E5 changes it afterwards, with a LoRA, and removes nothing from its parameters; it is the cheap neighbour of this line and does not replace it.

**Teaching or attaching a store.** MemLLM (arXiv:2404.11672) fine-tunes a model to use an explicit read-write memory, which is the closest published relative of E5. KBLaM (arXiv:2410.10450) leaves the model frozen and trains adapters that let it attend to encoded triples. Knowledge Objects (arXiv:2603.17781) benchmark in-context memory against structured fact objects.

**Retrieval, popularity and when to look up.** PopQA (arXiv:2212.10511) showed that parametric recall fails on the long tail; P1 and E5 use it. Whether a model should look up at all, and the effect of teaching it to, is the question of the `abstain_default` reading of §5.1.4.

**Memory versus reasoning, by layer.** Jin et al. (arXiv:2411.13504) and the layer-localisation papers (arXiv:2510.02091, 2510.03366, 2503.23084) separate recall from reasoning inside the network; they motivate E6.

**Gap.** The claim of novelty the plan carried was about P1. For this paper, `[UNVERIFIED: whether a lookup LoRA trained with a no-call control and held-out relations has been reported; no literature search beyond the plan's list was made and none is possible without the web]`. We claim no priority.

## 3. Setup

### 3.1 What is recapped from P1
Questions: the 1,000 unambiguous PopQA items of P1 §3.1 (stratified by popularity, dataset seed 1); metric `contains`; harness, serving flags and hash-locked model list as in P1 §3.2–3.3, §4.1, §8. The `tool` condition and the default prompt `SYSTEM_TOOL` are those of P1. Base models: Qwen3.5-2B and 0.8B, IQ4_XS GGUF, plus 2B Q8_0 for a transfer row.

### 3.2 What E5 adds
- **Adapters.** LoRA on all linear layers, trained in bf16 on the Hugging Face safetensors of each model (downloads approved by julio, 2026-11-20; sizes and sha256 recorded in the notebook), converted to GGUF (llama.cpp's converter) and served with `llama-server --lora` on the *same* IQ4_XS file as the baselines, so base and adapter rows differ only by the adapter. `models.lock` lists every adapter (id, hash, base, arm, recipe); the runner refuses an applied adapter that is not named, listed, hash-equal and built for the served base, and refuses a run with no `--expect-adapter` if any adapter is applied.
- **Arms.** Per model: **base** (P1's cells), **L1** (lookup trajectories), **L0** (same questions, same tools block, same final answers, direct answer, no call).
- **Training data, no model writes it:** see §5.1.1.

### 3.3 Hardware
As P1 §3.6: one RX 9060 XT 16 GB, production 27B stopped for each window by `ko_swap.py`. Training uses a separate environment (`~/lmlm/venv-train`: transformers 5.23.0, peft 0.22.0, accelerate 1.16.0, 15 GB on disk, measured); `flash-linear-attention` and `causal_conv1d` were not installed, so Qwen3.5's linear-attention layers ran the reference PyTorch path (the library warns it is "correct but much slower"); the speed with those kernels was not measured (notebook, 2026-11-20 and 2026-11-21).

## 4. Methods

### 4.1 Uncertainty and the pre-registered tests
All intervals are 95% percentile bootstraps over questions (10,000 resamples, seed 0), paired where two arms answer the same questions. One run per arm, greedy. **Greedy decoding is reproducible on this server in the one repeat measured**: the base model run twice on 500 GSM8K problems (2B IQ4_XS, same server) gave discordance 0.000 and 500 of 500 identical texts (`E5_TABLE.md`). One repeat, one server build; no claim for other builds.

### 4.2 The E5 family, fixed in `E5_DESIGN.md` on 2026-11-20 (before any training)
- **H9a (it looks up):** 2B L1 default-prompt lookup rate ≥ 0.80 on the full 1,000 **and** on the held-out relations.
- **H9b (lookup, not format, is what helps):** acc(L1, `tool`) − acc(L0, `tool`): CI lower bound > 0 and point difference ≥ +0.10.
- **H9c (skills intact):** GSM8K `closed`, L1 − base, 95% CI inside ±0.05.
- **Reported only:** closed-book cost, hallucination on absent facts, held-out against seen relations, paraphrase set P, three prompt wordings, Q8_0 transfer, the whole 0.8B arm. Abstention is not taught.
**The p-value definitions** (H9a: the larger of the two parts' shares of resamples at or below 0.80; H9b: share of paired resamples at or below 0; H9c: TOST, the larger of the two one-sided shares) **were written in `stats_e5.py` by the coordinator after the data existed and before any number was read**; the design fixed the decision rules and not these definitions. They were reviewed by the author and the script was rerun: its output is identical to `E5_TABLE.md`. A "holds" additionally requires Holm-adjusted p < 0.05, which is stricter than the design's text for H9c (see §5.1.2).

### 4.3 Holm: two families, and when each was written
The pre-registered confirmatory family of the whole programme is **m = 6 (H1, H2, H3, H4, H5, H7)**. H9a–c are not in it: they were added with the E5 design on 2026-11-20 as **their own family, m = 3**, after the main sweep and before any E5 training. The cut into two papers was decided on **2026-11-22, after the E5 data**; **this paper reports the E5 family's adjusted p-values as its headline for E5 (m = 3)** and also notes that H4 and H7, which would sit with it, are untested (p = 1). With H4 and H7 at p = 1 the E5 adjusted values for H9a and H9b rise only if the family is enlarged to 5 (raw p 0.0001 gives 0.0005 for both; H9c 0.24 × 3 gives 0.72, computed here); all three verdicts are unchanged either way. (This enlargement is shown for transparency and was not pre-registered.)

### 4.4 How the work was organised
The protocol of P1 §4.7 (roles, rules of work, files as channels, failures) applies unchanged; see P1. What is specific to E5 is told in §7 item 9 and in `NOTEBOOK.md` (entries of 2026-11-20 to 2026-11-22), including the gates that stopped a run before it started.

## 5. Results

### 5.1 Teaching the lookup with a LoRA (E5, route C)

P1 (§5.3 and §5.4) showed that whether a small model looks up by itself depends on a prompt, and that the prompt that works at 9B and at 2B Q8_0 does not work at 2B IQ4_XS (21% looked up under the contract) or at 0.8B (0%). E5 asks whether a small LoRA can supply that behaviour without a prompt. Design, written before any training and dated 2026-11-20: `E5_DESIGN.md` (route C, chosen by julio from five options). **What E5 is not:** it changes behaviour, not parameters. It removes no knowledge, so it says nothing about freed capacity and tests nothing of H4 (§5.2 stays open). **It does not replace LMLM.**

**5.1.1 What was done.**
- **Models and arms.** Qwen3.5-2B IQ4_XS (primary) and 0.8B IQ4_XS (secondary, reported separately, no pooling). Per model: **base** (the existing cells), **L1** (lookup trajectories) and **L0**, the control: same questions, same tools block, same final answers, but the model answers directly with no call. L0 is what lets a gain be attributed to looking up rather than to the short-answer format or PopQA's templates. Adapters were trained on the bf16 weights and served on the same IQ4_XS GGUF the baselines used (`--lora`), so base and adapter rows differ only by the adapter.
- **Data, no model writes it.** PopQA's 14,267 triples; 1,000 lookup examples per model (999 written, one dropped ungrounded) plus 500 anchors: the base model's own greedy answers to GSM8K **train** questions, half rendered with the tools block and half without, truncated answers dropped. No eval id and no triple sharing a subject URI with an eval item is in training (asserted in code). Three relation units were held out of training by a draw fixed beforehand (`Random(5)`: director, father, screenwriter, 244 eval items). The 1e5 popularity bucket had only 153 pool items for 195 wanted, so the other buckets were topped up (no repeats; julio's decision). Loss is on assistant tokens only; **the final answer's value is not masked** (flagged deviation: copying from the result is the skill taught).
- **Hyper-parameters.** Three dev settings, frozen before the first run (A r16/α32/lr 2e-4; B r8/α16/lr 1e-4; C r32/α64/lr 2e-4); fixed: 2 epochs, 376 steps, batch 8, seed 0. On 300 dev items all three looked up 1.000; the rule frozen beforehand (tie → dev accuracy) selected B (accuracy 0.990 against 0.987, one item). The stopping rule (retrain at 3,000 + 1,500 if dev lookup < 0.80) did not fire.
- **Checks before the eval cells.** Real chat template against the server's `/apply-template` and token-level mask decode (gates 9a/9b) passed. For each of the four adapters, at eval time: scale 0 reproduces the base replies exactly and scale 1 differs. `verify_e5.py` passed 21 of 21 cells before any statistic was computed. Adapter and base hashes are in `models.lock`.
- **Cost (measured).** Training wall: 2B L1 1,425 s, L0 1,399 s; 0.8B L1 976 s, L0 896 s; peak 9.34, 8.99, 5.13, 4.79 GiB; seven trainings in all about 2.4 h of GPU. The eval window took 2 h 44 min. Production was stopped for each window.

**5.1.2 The pre-registered family (Holm m = 3, own family; the main family's m = 6 is unchanged).** Raw p-values come from 10,000 question bootstraps, seed 0, one-sided as in `stats_holm.py`. **The p-value definitions (H9a: the larger of the two parts' shares of resamples at or below 0.80; H9b: share of paired resamples at or below 0; H9c: TOST, the larger of the two one-sided shares) were written in `stats_e5.py` by the coordinator after the data existed and before any number was read; the design fixed the decision rules (below) but not these definitions.** I reviewed them and reran the script: its table is reproduced byte for byte.

| test | rule written in the design | result | raw p | Holm adj. p | verdict |
|---|---|---|---|---|---|
| H9a it looks up | 2B L1 default-prompt lookup ≥ 0.80 on all 1,000 **and** on the held-out relations | 0.992 [0.986, 0.997] on 1,000; 1.000 on the 244 held-out | 0.0001 (resampling floor) | 0.0003 | **holds** |
| H9b lookup, not format | acc(L1) − acc(L0) on `tool`: CI lower bound > 0 and difference ≥ +0.10 | +0.705 [+0.676, +0.733]; L0 looks up 0.000 and answers 0.285, L1 answers 0.990 | 0.0001 | 0.0003 | **holds** |
| H9c skills intact | GSM8K `closed`, L1 − base, 95% CI inside ±0.05 | −0.034 [−0.074, +0.006] (accuracy 0.726 → 0.692) | 0.24 | 0.24 | **does not hold** |

**5.1.3 Reading the three tests.**
- **H9a.** Under the default prompt, where the base looks up 0.000 times in 1,000 questions, the adapter looks up 992 times; on the three relations it never saw, 244 of 244. The held-out interval printed by the script is a degenerate [1.000, 1.000], which is not a claim of zero uncertainty; the exact one-sided 95% lower bound for 244 of 244 is about 0.985 (0.025^(1/244), computed by hand for this paragraph, not by the script). The two p-values sit at the bootstrap's floor 1/10,001. 0.8B (reported only, not in the family): lookup 0.995 on all 1,000 (accuracy 0.979) and 0.996 on the held-out.
- **H9b.** The size of the difference is not informative about learning: L0 never looks up, answers from the weights (0.285 against the base's 0.245 on `tool`), and L1 gets the answer from the store. H9b tests that the gain is not the format; it did, with a margin of +0.705 over the +0.10 bar. A model-side claim would be a mistake: **accuracy of about 0.99 under lookup is the store returning the gold fact** (the 9B under a prompt contract reached 0.991 as well, P1 §5.4), not knowledge in the weights.
- **H9c does not hold, and it is not a demonstrated loss.** The interval includes zero (upper bound +0.006) but its lower bound (−0.074) is outside the margin, so equivalence is not shown. The 90% interval (−0.068, 0.000, computed by me with the script's bootstrap) is also outside −0.05, so the verdict does not depend on how the TOST level is read (the gate "95% CI inside ±0.05" corresponds to one-sided tests at 2.5%, stricter than the "adjusted p < 0.05" gate the script adds; both fail). The margin was **attainable**: the base-vs-base repeat (GSM8K `closed`, 2B IQ4_XS, 500 problems, same server) gave a discordance of 0.000 and **500 of 500 identical generated texts**, so greedy decoding is reproducible here and the 20.2% of problems on which L1 and base disagree (42 problems L1 gets right and base wrong, 59 the other way) come from the adapter, not from noise. The design's attainability condition (discordance ≤ about 0.325) was met, but it is a necessary condition for a true difference of zero only: at the observed discordance of 0.202 the interval half-width is about 0.039, so equivalence to ±0.05 could only have been declared if the point estimate lay within about ±0.011 (my calculation, normal approximation, not measured). One repeat, one server build; no claim for other builds or batch sizes.

**5.1.4 Reported only (not in the family, no p-values).**
- **The control moves too, post hoc and exploratory.** Same bootstrap, committed cells, computed by me after seeing the table, **not pre-registered**: at 2B, L0 − base = −0.016 [−0.052, +0.020] and L1 − L0 = −0.018 [−0.048, +0.012]; at 0.8B, L1 − base = −0.088 [−0.134, −0.044], L0 − base = −0.086 [−0.132, −0.042] and L1 − L0 = −0.002 [−0.032, +0.028] (accuracies 0.436 / 0.348 / 0.350). At 0.8B the whole drop is shared by both arms, so it is not specific to lookup; at 2B the L1 − L0 difference is not distinguishable from zero. Both arms share the same LoRA recipe and anchors, so these data say "this recipe lowers GSM8K `closed` somewhat", and cannot say why. No mechanism is claimed.
- **Closed book.** 2B 0.243 → 0.238 (L1 `closed`); 0.8B 0.213 → 0.182. The adapter did not make the model stop answering from its weights when no tool is offered.
- **Paraphrase set P (200 items, 2 hand-written templates per relation, hashed before training).** 2B: L1 0.965 (lookup 0.950) against L0 0.260; 0.8B: 0.980 against 0.185.
- **Other wordings** (L1 was trained on `SYSTEM_TOOL` only): `tool_free` 2B 0.983, 0.8B 0.963; `tool_describe` 0.993 and 0.973.
- **Knowing when not to look up: not taught and not achieved.** With the `abstain_default` prompt (n = 200) 2B L1 looks up 1.000, answers 0.195, abstains 0.265, hallucinates 0.540 (0.8B: 0.640). Abstention was deliberately not part of the training (single variable).
- **Quantisation transfer.** The same 2B adapter on the Q8_0 base: accuracy 0.995, lookup 1.000. The scale-0 gate was run on the adapter's own base, not on Q8_0.
- **By relation and by popularity.** At 2B every relation looks up at 0.946 or more (lowest: `capital`, 0.946 of 130); by popularity bucket the lowest is 1e5, 0.969 of 194. The rows of 1e6 and 1e7 have 8 and 2 items.

**5.1.5 What this supports and what it does not.**
- It supports: at 2B and 0.8B IQ4_XS a LoRA trained on 999 lookup examples, one seed, makes the model call `lookup` on 0.992 (2B) and 0.995 (0.8B) of the 1,000 questions under the default prompt, including on three relations absent from training, where the prompt contracts of P1 §5.4 had left 2B IQ4_XS at 21% and 0.8B at 0%. The control shows the accuracy gain comes from lookup and not from the answer format.
- It does not support: that the skill cost is nil (H9c is not met at 2B and the 0.8B drop is shared with the control); any statement about knowledge removed or freed capacity (H4 untested; §5.2); that the model knows when not to look up; that the result generalises across relations of a different kind, templates not seen (set P is partial evidence), tools other than `lookup`, or model families.
- Limits: **one seed and one run per arm**, so no variance across training seeds exists; the eval is in-distribution for templates and popularity profile; `director` and `screenwriter` are close to relations that were trained (`producer`, `author`, `composer`), so **`father`** is the clean held-out relation (50 items, all looked up); training in bf16 and serving in IQ4_XS were not compared numerically; the Q8_0 row was not gated on Q8_0; the 0.8B set of cells and the use of `transfer_bases` are choices julio confirmed after the fact; per-question payloads exist on yoda only (64 MB, not committed). `[TBD: a second training seed]` `[TBD: a held-out relation of a different kind, or a different tool]` `[TBD: the cause of the GSM8K shift, if julio wants it known: it would need an ablation not run]`.


### 5.2 Models with knowledge removed (E1b, H4): prerequisites only

This section covers the optional second track: whether pre-training or editing that removes facts from a model changes what its parameters do (LMLM and Co-LMLM against their standard twins; layer ablation; a lookup LoRA). **julio decided on 2026-11-17 that this line of work is wanted. The first route, a lookup LoRA, was chosen on 2026-11-20 and is §5.1. The route that changes pre-training (LMLM-style) is not decided, and no experiment on these models has been run.** What exists is a set of prerequisite measurements on one released pair, `LMLM-llama2-382M` and `Standard-llama2-382M`, on the machine of P1 §3.6. They show whether the track is feasible here and at what cost. **They say nothing about whether removing knowledge works, about LMLM's accuracy, or about any of our tasks.** When this route is run, this section is rewritten from its own pre-registration; H4 (non-fact perplexity) belongs here and stays at p = 1 until then (§4.3).

**5.2.1 What was measured (notebook, entries of 2026-11-18 and 2026-11-20; all single runs).**

| item | result | measured / estimated |
|---|---|---|
| LMLM's pinned `torch==2.4.0` on this machine | not installable: yoda has only Python 3.13.5, and the ROCm index lists no 2.4.0 build for it (only 2.6.0+rocm6.1 for cp313) | measured (`pip download`) |
| Environment used instead | torch 2.12.1+rocm7.2, transformers 4.49.0 (pin kept), faiss-cpu 1.16.0, numpy 2.5.5, marisa-trie 1.4.1 (pin 1.2.1 not tried); for training, `trl==0.15.2`, which the repo does not pin and which was chosen by date | measured (`freeze.txt`); **the torch version differs from the authors'** |
| `LMLM-llama2-382M`, repo example, GPU (RX 9060 XT) | rc 0; 58.3 s for 6 prompts (max 512 new tokens, up to 10 lookups); VRAM peak 3,228,696,576 B (592,822,272 B already in use before, so about +2.64 GB); max RSS 3,289,544 kB | measured, one run |
| `Standard-llama2-382M`, same example | CPU (the repo's code path): 185.4 s; max RSS 2,697,688 kB; VRAM unchanged | measured; **different device from the row above, so not a model comparison** |
| Does it fit beside the production 27B? | no: the 27B holds 14,981,218,304 B of the card's 17,095,983,104 B (about 2.1 GB free) and the run needs about 2.64 GB more. A first attempt with the 27B serving ended in the kernel's out-of-memory killer about 40 s in and the machine stopped answering until julio rebooted it (P1 §4.7, item 6); that run produced no result | measured (journal, `vram.log`); any LMLM run needs production stopped |
| Do the lookups happen? | yes: each of the 6 prompts contains 9 `<|db_entity|>…<|db_return|>…<|db_end|>` calls. The database is `dwiki_bio17k` (biographies). The one entity checked against the database (Ko Itakura) returned a plausible entry; for entities not in it, with `fallback_policy top1_anyway` the database returned an unrelated entry with no sign of doubt (Mao: "Paper Tiger"; Fábio A L de Souza: "Master's degree in Jazz Studies") | measured on 6 prompts; **not a rate** |
| Full database `dwiki6.1M` (54.6M triples) | downloaded, 7,921,290,218 B (equals the Hugging Face tree size); `json.load` under a 12 GB address-space cap fails with `MemoryError` after 13.7 s at max RSS 7,745,484 kB | measured; shows it does not fit under that cap, **not** that it does not fit in the machine's 15.8 GB; loading without the cap, entry count and the FAISS index were **not tested** |
| Training throughput with the repo's own trainer (`lmlm.training.pretrain`, `pretrain.sh` arguments, tiny-llama2-382M, packing, sequence 1024, gradient checkpointing, bf16, 30 steps, one GPU, 200 MB head slice of the corpus) | batch 8: 5,353 tokens/s (45.9 s); batch 16: 5,675 tokens/s (86.6 s); batch 32: out of memory. VRAM peak 16.07 GB and 16.42 GB (includes 0.6 GB used by other processes) | measured; 30 steps with warm-up, first steps included |
| Hours to reproduce the pre-training | **about 45 days** (2.20e10 tokens = 28,022,521,773 B × 0.0983 tokens/byte × 8 epochs, at 5,675 tokens/s; 47.6 days at 5,353) or **about 32 days** (1.57e10 tokens, from the checkpoint name `…ep8_bsz256…/checkpoint-60000` = 60,000 × 256 × 1,024; 34.0 at 5,353) | **estimated, not measured.** The tokens-per-byte ratio comes from the first 2,001 documents only; the two token counts disagree by 40% and which one the released model used is not known; the original script used 2 GPUs at batch 128 each, which this card cannot reproduce; throughput of 30 steps is extrapolated to weeks |

**5.2.2 What this supports and what it does not.**
- It supports: the released 382M models load and generate on this card under a newer torch than the authors'; the lookup syntax is produced and answered by the database; running them requires stopping production; reproducing the pre-training from scratch on this one card would take weeks, not hours (an estimate with the caveats above).
- It does not support any statement about accuracy, about whether the parameters hold fewer facts, about Co-LMLM, about layer ablation or about a lookup LoRA: none was run. The repo itself states about 8 H100-days for the 382M; we did not check that figure `[UNVERIFIED]`.
- `[TBD: LMLM and Standard evaluated on a task of ours; the full database loaded without a cap and its FAISS index; the training throughput at a batch size that matches the authors'; whether a smaller reproduction (fewer tokens) is informative]`.
- The `top1_anyway` observation is a property of the database fallback setting on 6 prompts, not a finding about the method; whether the model can decline to use a wrong return is untested.


### 5.3 `[OPEN]` Layer ablation (E6)
Whether removing layers that carry recall leaves reasoning intact (the layer-localisation papers of §2 suggest where to look) was not designed in code, not run and not costed: its cost is not measurable without a probe. `[OPEN: scope, julio]`.

### 5.4 `[OPEN]` Does an agent fit a smaller model? (E7, H7)
The title's question: with the facts in a store, is a model within a memory budget (8 GB) within five points of the 27B on a skill suite of the agent's real jobs? `[TBD: the Funes skill suite does not exist; size sweep with the store; H7]`. Estimated cost after the suite is built: 2–8 h of GPU (notebook, 2026-11-22; an estimate). H7 enters Holm as p = 1 and **nothing in this paper bears on it**. The partial evidence P1 holds (a 0.8B model with a store is above the 27B closed-book on PopQA; the `funes` rows are far below the store at 0.8B and 2B) is about facts, not about the agent's skills.

## 6. Cost
Measured: E5 training wall 1,425 s (2B L1), 1,399 s (2B L0), 976 s (0.8B L1), 896 s (0.8B L0), peak VRAM 9.34, 8.99, 5.13, 4.79 GiB, about 2.4 h of GPU for the seven trainings (three dev settings plus four final), and a 2 h 44 min eval window (notebook, 2026-11-21); LMLM prerequisites in §5.2. Estimated, not measured: training time had the 3,000 + 1,500 retrain been needed (about 16 h, from a 30-step probe; the retrain was not needed, notebook 2026-11-20); reproducing LMLM's pre-training (§5.2). Not measured: serving VRAM with the adapter beyond what the baselines needed; energy.

## 7. Limitations
1. **One training seed per arm and one repeat.** No variance across training seeds exists. The verdicts of H9a and H9b are far from their bars; H9c's is not, and a second seed could move it either way. Second seed: explicit future work.
2. **In distribution.** Eval questions share PopQA's templates and popularity profile with training. The held-out relations and set P exist to expose this partly; `director` and `screenwriter` are close to relations that were trained (`producer`, `author`, `composer`), so `father` (50 items, all looked up) is the clean held-out relation.
3. **Accuracy under lookup is the store's, not the model's.** A reader must not read 0.99 as knowledge in the weights.
4. **The GSM8K shift has no tested cause.** The control moves too; the recipe (rank, rate, anchors, epochs) was not ablated.
5. **bf16 training, IQ4_XS serving.** Not compared numerically. The Q8_0 transfer row was not gated on Q8_0.
6. **The 0.8B set of cells and `transfer_bases` were assumptions that julio confirmed after the fact** (notebook, 2026-11-21).
7. **One model family, one tool,** one prompt for training. Other wordings are reported only for L1.
8. **Per-question E5 payloads exist on yoda only** (64 MB); the committed cell JSONs and tables are enough for the statistics but not for every audit.
9. **The same family reviewed itself,** as in P1 §4.7; E5 went through its own gates (data builder review, chat-template and mask checks, scale-0 identity per adapter, a verifier before any statistic) and the review by the author of this draft, a model.
10. **Knowledge-removed models: prerequisites only.** The 382M LMLM pair complete biographies rather than follow instructions; they bear on whether masking changes the parameters, not on whether an agent could run on one.
11. **Post-hoc elements.** The p-value definitions of §4.2 and the exploratory bootstrap of L0 and L1 in §5.1.4 were written after the data existed; the Holm family split of §4.3 after the data too.

## 8. Reproducibility
**Released with the paper:** the harness and `E5_DESIGN.md`, `e5_build.py`, `e5_train.py`, `stats_e5.py`, `verify_e5.py`, `e5_paraphrase_templates.json` (hashed before training: `c455152a07209788b9fd46ab2c8c4dfe5edf99debad040035289821ae553b6a9`, which is the canonical sha256 computed by the author of this draft and recorded by the coordinator before training; recheck with `e5_build.py --templates-sha256`), `models.lock` with the adapter entries, every committed cell JSON and `E5_TABLE.md` (sha256 `ee79f63c0fc24e6dd498f6cee6e93a5ed4bd23e9e105c265dc4de9bfad0284af`), and the gate files `results/e5/gate_*.json`. A figure can be rebuilt from the JSON without a GPU. **Not released / not committed:** training data files (rebuilt deterministically by `e5_build.py` from `data/popqa_test.tsv`; the manifest records every file's sha256), the per-question payloads (yoda), the adapters' weights beyond their hashes `[TBD: decide whether to release the four GGUF adapters, 10.8–16.8 MB each]`. The environment of P1 §8 applies, including its `[UNVERIFIED]` power cap. Windows: `results/ko_swap-*.log` and the notebook entries of 2026-11-20 and 2026-11-21.

---

*Count (grep of this file, 2026-11-22): 8 `[TBD` tags, 7 `[OPEN`, 6 `[UNVERIFIED` (these include mentions in status tables and the format example in the header); recount with grep.*
