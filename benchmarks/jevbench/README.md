# JevBench — benchmarking the `classifier` agent

Runs the public [JevBench](https://github.com/fstandhartinger/jevbench) dataset
against Funes's `classifier` agent (`classify_decision` tool) to see how it
compares to TypeSafe's Jev and to Reflex, the open-weights reimplementation
that started this line of work — see the conversation this came out of for
that comparison. Not part of `ctest`: it needs a live LLM, is slow (network
round trip per decision), and its numbers depend on which model is serving,
so it doesn't belong in the deterministic, hermetic test suite.

## What's vendored here

`datasets/{original,easy,hard}.jsonl` — 231 of JevBench's public decisions
(72 + 48 + 111), fetched from
`github.com/fstandhartinger/jevbench@main` on 2026-09-21. MIT-licensed per
the source repo (`"MIT for this harness and the 72 original public
decisions"`) and per each record's own `provenance.license` field. The
private/held-out portions of the standard, judge and hard tiers (133+
further items) are not published anywhere and are not here.

Each record looks like:

```json
{
  "id": "original-policy-01-0",
  "family": "policy",
  "state": "Policy: refunds require a receipt...",
  "question": {"type": "noul", "instructions": "...", "criteria": {"true": "...", "false": "..."}},
  "labels": ["no", "yes"],
  "expected": "no"
}
```

`question.type` is one of `noul` (binary), `choice` (multi-way) or `score`
(ordinal — `criteria` is a list aligned with `labels`, `expected` is an
integer index). `run_benchmark.py` folds `question.criteria` into the
`Question:` line it sends, since the agent's system prompt only knows to
parse `State:` / `Question:` / `Options:` — the rubric is real information a
fair comparison has to give the model, so it isn't dropped, just inlined.

## Running it

The script talks to a *running* Funes instance over its normal HTTP API —
it doesn't touch the database or the LLM directly. Point it at a **scratch**
instance, not your real production account: each decision becomes a real
chat turn (and, if `auto_memory` is on for the account, a real memory), and
231 of those is not something you want cluttering a real conversation
history. The cheapest way to get one, reusing the exact recipe this feature
was verified with:

```bash
# on yoda (or wherever the model you want to benchmark is actually running) —
# no SSH tunnel needed if you run this on the same host as llama-server
cd ~/Funes
DB=$(mktemp -u /tmp/funes_bench_XXXX.db)
WS=$(mktemp -d /tmp/funes_bench_ws_XXXX)
FUNES_DB="$DB" FUNES_WORKSPACE_DIR="$WS" FUNES_AGENTS_DIR="$(pwd)/agents" \
FUNES_PORT=18485 ./bin/funes &

curl -s -X POST http://127.0.0.1:18485/api/auth/bootstrap \
  -d '{"username":"jevbench","password":"<pick one>","display_name":"JevBench"}'
```

Then, from wherever `run_benchmark.py` runs (needs only stdlib Python 3, no
`pip install`):

```bash
# smoke test first — 5 items, seconds not minutes
python3 run_benchmark.py run \
  --dataset datasets/original.jsonl \
  --base-url http://127.0.0.1:18485 \
  --username jevbench --limit 5 \
  --output /tmp/jevbench_smoke.jsonl

python3 run_benchmark.py summarize \
  --dataset datasets/original.jsonl \
  --results /tmp/jevbench_smoke.jsonl

# the real run — all 231 public items. Measured against yoda's
# Qwen3.8-9B-Q8_0 (2026-09-21): ~12-19s per item — a full /api/chat round
# trip through the agent loop, not classify_decision's own two forward
# passes (see "What's measured" below) — so expect on the order of an hour,
# not minutes. This is real load on whatever LLM backs the instance — don't
# point it at a shared production model during hours real usage matters.
python3 run_benchmark.py run \
  --dataset datasets/original.jsonl datasets/easy.jsonl datasets/hard.jsonl \
  --base-url http://127.0.0.1:18485 \
  --username jevbench \
  --output results/run-$(date +%Y%m%d).jsonl

python3 run_benchmark.py summarize \
  --dataset datasets/original.jsonl datasets/easy.jsonl datasets/hard.jsonl \
  --results results/run-$(date +%Y%m%d).jsonl \
  --report results/run-$(date +%Y%m%d)-summary.json
```

`run` and `summarize` are separate on purpose, matching the real JevBench
CLI's own `run`/`summarize` split: re-scoring never needs another round trip
to the LLM, so a new metric or a bug fix in `summarize_group` can be
re-applied to an old `results.jsonl` for free. `results/` is gitignored —
raw run output is a point-in-time artifact of one model/config, not
something to commit.

## What's measured, and what isn't

Computed, following JevBench's own documented definitions as closely as a
reimplementation reasonably can:

- **Accuracy** — argmax over the exact label set, matching `expected`.
- **Majority-class accuracy** — the score of always guessing the commonest
  label in that group; the floor any of these numbers has to clear to mean
  anything.
- **Brier score** — `sum_k (p_k - y_k)^2` over the exact label set, using
  `classify_decision`'s own reported probabilities (not just the argmax) —
  this is the metric this whole design exists for, since a system that
  only returns a label as text couldn't be scored this way at all.
- **ECE** — top-label confidence, 10 equal-width bins, empty bins excluded.
- **Ordinal MAE** — for `score`-type items, the probability-weighted level
  against the reference level.
- **Invalid rate** — a chat call that errored, timed out, or didn't return
  parseable JSON (the `answer_schema` contract makes this rare in practice —
  see the `classifier` agent's own verification history — but it's tracked
  rather than assumed away).
- **Latency** — wall-clock per decision, mean and p90. This is *not*
  comparable to Reflex's or Jev's published 200ms/1s numbers: it's a full
  `/api/chat` round trip through the agent loop (system prompt, the tool's
  two forward passes, `answer_schema` validation, and a possible retry), not
  a single stripped completion call.

Not reproduced: JevBench's composite **"JevBench Score"**
(Intelligence/Calibration/Speed/Cost, 25% each). Its Speed and Cost axes are
defined against commercial hosted-API assumptions ($/M tokens, a vendor's
published latency) that don't translate to a self-hosted model already
running for other reasons — reporting a cost or speed "score" here would be
a number, not a fact. Accuracy/Brier/ECE/MAE are the axes that actually
carry over to a self-hosted comparison, so those are what's reported.

## A caveat on tier labels

JevBench's own README describes a v1.2 dataset of 534 decisions across an
"easy/standard/judge/hard" structure, of which only three files are
publicly downloadable: `original.jsonl` (72, fully public — a distinct
authored cohort, not literally "the standard tier"), `easy.jsonl` (48 of 72
public), and `hard.jsonl` (111 of 220 public). The private standard/judge
tiers aren't published anywhere this tool can reach. Results here are
reported per source file and per `family` rather than mapped onto Reflex's
published "Easy / Standard / Hard" row labels, since that exact
correspondence isn't verifiable from the public repo alone — per-family
numbers are more honest than a guessed relabeling.
