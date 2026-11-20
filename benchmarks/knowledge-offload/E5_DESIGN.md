# E5 — teach lookup with a LoRA (route C): design for implementation

Written 2026-11-20 by Opus (design/audit), at julio's choice of route C. **No code, no model, no GPU, no download was done to write this.** Everything marked `[TBD]` is a measurement a session must make before the thing it blocks. Numbers below are from committed results unless marked.

## 1. Question and what it can and cannot say

Question: can a small LoRA make a model look up *by itself*, under the default prompt, where today it does not? Measured today (default prompt, 1,000 unambiguous PopQA questions, `H5H8_TABLE.md`):

| model | tool lookup (default) | tool+contract lookup | closed | tool acc | contract acc |
|---|---|---|---|---|---|
| 0.8B IQ4_XS | 0.000 | 0.000 | – | 0.191 | 0.190 |
| 2B IQ4_XS | 0.000 | 0.212 | – | 0.245 | 0.374 |
| 2B Q8_0 | 0.000 | 0.958 | – | 0.239 | 0.909 |
| 9B IQ4_XS | 0.001 | 0.989 | – | 0.347 | 0.991 |

So prompts already fix 9B and 2B Q8_0. They do **not** fix 2B IQ4_XS (21% looked up) or 0.8B (0%). Those two are where a LoRA could add something no prompt does, hence the choice below. The 9B is excluded (a prompt solves it; QLoRA of a 9B in 17 GB is unmeasured).

It can say: whether lookup behaviour is learnable at this size with this recipe, what it costs closed-book and on a skill, and whether it generalises past the training questions.
It cannot say: anything about freed capacity or knowledge removal (a LoRA changes behaviour, not parameters; this does not replace LMLM/Co-LMLM and tests nothing of H4), anything about sizes or families not trained, or anything about Funes's own tools. H4 stays untested and keeps p = 1 in the Holm family (m = 6 is not changed).

## 2. Models, arms, conditions

Trained: **Qwen3.5-2B** (primary) and **Qwen3.5-0.8B** (secondary, same recipe, reported separately, no pooling). Base for serving is the *same GGUF the baselines used* (IQ4_XS, `models.lock`), with the adapter loaded by llama-server (`--lora`), so base and LoRA rows differ only by the adapter. Training needs the bf16 safetensors of those two models: **a download julio must approve; size not measured**. Training on bf16 and serving on IQ4_XS is a mismatch, stated in the paper; the 2B Q8_0 row (same adapter, no retraining) shows whether it transfers across quantisation.

Arms (each trained once per model, same data order, same seed, same steps):
- **base** — exists (rows above, GSM8K rows).
- **L1** — trained with lookup trajectories (§3).
- **L0, the control** — trained on *the same questions and the same final answers*, with the lookup removed (question → answer directly). Without it, any gain of L1 could be the short-answer format or PopQA's templates, not lookup. L0 costs one more training run per model; dropping it is julio's call and means the paper cannot attribute a gain to lookup.

Eval cells (all greedy, one repeat, harness unchanged, system prompt = `SYSTEM_TOOL` exactly; the existing 1,000-item sample, ids frozen):
1. L1 and L0: `tool` default prompt (lookup rate, accuracy). L1: also `closed` and `tool` with the `abstain_default` prompt (n = 200 arm).
2. L1 and L0: GSM8K `closed`, the existing 500-item sample (skills).
3. L1: `tool` under the `tool_free` and `tool_describe` wordings (prompt generalisation; trained on `SYSTEM_TOOL` only).
4. L1: `tool` on the 2B Q8_0 base (quantisation transfer).
5. L1 and L0 on the **paraphrase set P**: 200 of the eval items re-asked with hand-written templates that differ from PopQA's (2 per relation, written and hashed before training, reviewed by Opus). `[TBD: Sonnet drafts, Opus reviews]`.
Measured base cost for scale: GSM8K `closed` 785 s (2B) and 485 s (0.8B); PopQA cell times `[TBD: sum from the committed base cells]`.

## 3. Training data (deterministic, no model writes it)

- **Source:** PopQA (the repo's `data/popqa_test.tsv`, 14,267 triples, 16 relations). **No download needed for this half.**
- **Disjointness (fixed before anything is trained):** exclude the 1,000 eval items and every triple that shares a subject URI (`s_uri`) with one. Hold out **3 relation units** never seen in training, drawn once by `random.Random(5).sample(...)` from the relations with at least 40 eval items, treating `capital` and `capital of` as one unit and excluding `color` (5 eval items). Print the draw and the eval counts per held-out relation before training; if the three together have fewer than 150 eval items, the rule is "redraw once with `Random(6)`", logged. Per-relation and held-out subset results are reported, not pooled away.
- **Lookup examples:** question → assistant calls `lookup(entity=<subject>, relation=<prop>)` → result from the harness's own `T.run_lookup` over the full 14,267-triple store (same K = 5, same distractors) → assistant answers with the object's first alias. Rendered with the model's chat template **with the same `tools` block and system prompt as at eval** (`SYSTEM_TOOL`), so the call format is the one `llm.py` parses. Popularity sampled to match the eval buckets (about 195 per bucket 1e1–1e5; `[TBD: pool counts per bucket]`).
- **Loss mask:** assistant tokens only: the call and the final answer. Tool results (the returned values) are not in the loss, which is what the plan's "mask the returned values" asks. **Deviation, flagged:** the final answer's value is *not* masked, because copying from the result is the skill being taught; memorising it cannot help eval since no training entity is an eval entity.
- **Anchor examples (one third of the mix):** non-fact prompts answered by **the base model itself** (its own greedy outputs, kept right or wrong), so they teach no new capability and push toward "behave as before when no lookup is needed". Prompt source: GSM8K `train.jsonl` questions, **a download julio must approve (same repository as the test file already used; size not measured)**. Not the GSM8K test split.
- **Size and stopping rule (written now):** start with 1,000 lookup + 500 anchor examples. If the dev lookup rate (below) is < 0.80, retrain once with 3,000 + 1,500. If still < 0.80, stop and report the negative; no further tuning.
- **Dev split:** 300 pool items (not eval, not training) pick the checkpoint and at most 3 hyper-parameter settings (rank, learning rate: values `[TBD: Sonnet lists them, frozen before the first run]`), by dev lookup rate then dev accuracy. **The eval items are looked at once per final adapter.**
- **L0 data:** identical questions, system prompt and answers; no tools block, no call.

## 4. Pre-registered decisions (E5 family, Holm m = 3, separate from the m = 6 of the main paper)

All paired, 1,000 items, bootstrap 10,000 with the harness's seed, one-sided p as in `stats_holm.py`.
- **H9a (it looks up):** at 2B IQ4_XS, L1's default-prompt lookup rate ≥ 0.80 on the full eval set **and** ≥ 0.80 on the held-out relations. (0.80 is D12's bar.)
- **H9b (lookup, not format, is what helps):** acc(L1, `tool`) − acc(L0, `tool`) has a lower CI bound > 0 **and** a point difference ≥ +0.10.
- **H9c (skills intact):** GSM8K `closed`, L1 − base, 95% CI inside ±0.05. (Margin chosen without a power calculation; `[TBD: Sonnet computes the base-vs-base discordant rate from the committed cells and reports whether ±0.05 is attainable at n = 500; if not, say so, do not widen the margin after the data]`.)
- **Not in Holm, reported only:** closed-book cost acc(L1,`closed`) − acc(base,`closed`) (the plan's "stopped trusting itself"); hallucination on absent facts (the `abstain_default` arm); held-out vs seen relations; paraphrase set P; the three prompt wordings; Q8_0 transfer; the whole 0.8B arm. Abstention is **not taught** here (single variable); teaching it is a different experiment and julio's call.
- **If GSM8K rises:** reported as a rise, with the base-vs-base noise next to it and no mechanism claim.

## 5. Order of work and gates (who)

**Decisions by julio, 2026-11-20:** (1) the downloads are approved: Qwen3.5-2B and 0.8B bf16 weights and GSM8K `train.jsonl` (record each file's size and sha256 on download); (2) training runs on this card, **unless it takes days**; (3) L0 stays; (4) the 0.8B arm runs. *My reading of (2), to be confirmed:* after the throughput probe of step 3(a), if the estimated GPU time of all four trainings (2B and 0.8B, L1 and L0, with the 3,000-example retrain counted) is over 24 hours, the run stops and julio decides between waiting and a rented GPU; the estimate is marked "estimated" until measured. Production is stopped for each GPU window as for the LMLM tests, with julio's OK per window.

1. **julio:** done (above).
   **Coordinator:** the downloads (read-only on everything else).
2. **Sonnet, no GPU:** the data builder (§3) with tests (disjointness asserted in code: no eval id, no shared `s_uri`, no held-out relation in training; template round-trip: rendered training example parses with `llm.py`); print the held-out draw and counts. Opus reviews the draw and the paraphrase templates before any training.
3. **Sonnet + coordinator, GPU:** (a) a 30-step training throughput and VRAM probe for 2B and 0.8B (the LMLM probe's method; **no training-hours estimate exists today**); (b) LoRA → GGUF conversion and `--lora` serving on the IQ4_XS base: *scale-0 adapter must reproduce base outputs identically on 20 items* (measured gate), and a non-zero adapter must change them; (c) adapter sha256 and base sha in `models.lock`, runner gate extended.
4. **Coordinator, GPU:** anchors (base generates its own answers), train L1 and L0, cells of §2. All through the verifier pattern (`verify_*.py` before any statistic).
5. **Opus:** reads the table, applies §4 as written, writes the paper section.

## 6. Risks stated now
- The adapter may not load or may drift on IQ4_XS (llama.cpp path unmeasured here): gate 3(b) decides; fall back to serving the Q8_0 base for both arms if needed, and say so.
- At 0.8B the model may be unable to emit a valid call even after training; that is a result.
- In-distribution: eval questions share PopQA's template with training; P and the held-out relations exist to expose that, not to remove it.
- One seed, one run per arm: no variance across training seeds is measured. Stated as a limitation; a second seed is julio's call.
- The mismatch between bf16 training and IQ4_XS serving is not separated from the LoRA effect except via the Q8_0 row.
