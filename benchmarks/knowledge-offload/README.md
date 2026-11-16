# knowledge-offload — E0 harness

Research code for `docs/knowledge-offload-plan.md` (the source of truth for
what is measured and why). Python, stdlib-only harness, its own venv, not in
`ctest`: it needs a live model on yoda. Modelled on `benchmarks/jevbench`.

```
run.py            fetch | run | summarize
ko/popqa.py       PopQA load + seeded sample + popularity bucket
ko/scoring.py     alias match (PopQA's metric) + exact + abstain flag
ko/store.py       triples in SQLite + FTS5 (BM25) + exact (entity, relation) lookup
ko/llm.py         llama-server client; cost from its `timings`; refuses loopback
ko/tools.py       lookup / web_search / web_fetch / recall / remember
ko/conditions.py  the eight conditions, the shared prompt and caps
ko/funes.py       scratch Funes instance for the `funes` condition
ko/runner.py      one cell -> one result JSON
ko/stats.py       bootstrap CI, paired bootstrap, McNemar, repeat spread
tests/            hermetic unit tests (fake LLM, fake web)   python3 -m pytest tests
```

## Run

```bash
./setup_venv.sh                    # separate venv (E1b later adds ROCm torch here)
python3 run.py fetch               # PopQA test.tsv -> data/ (sha256 goes into every result)
python3 -u run.py run --model-tag qwen3.8-9b-q8 --expect-model 9B \
    > results/run.log 2>&1         # 1k sample, 8 conditions, 3 repeats, resumable
python3 run.py summarize
```

`--expect-model` is not optional: the run aborts unless the file yoda's
:8080 serves contains it. A model swap on yoda is a logged, undone step
(plan, Rules) done by hand outside the newsletter/noticiero cron windows —
never a side effect of a run. Cells already on disk are skipped (`--force`
redoes them), so an interrupted run resumes.

## Conditions, as implemented

All share one system prompt, greedy decoding (temperature 0, seed = `--seed` +
repeat), `max_tokens` 128, k = 5, at most 4 tool rounds; the last round
withholds tools (Funes's last-step reservation). Constants live in
`ko/conditions.py`; changing one is a new row in the notebook.

| condition | what the model gets |
|---|---|
| `closed` | the question |
| `oracle` | question + the gold triple |
| `store` | question + top-5 BM25 triples for the question text (harness retrieves; `retrieval_hit` recorded) |
| `tool` | `lookup(entity, relation)`, model decides |
| `tool+contract` | same, but a text answer is refused (≤ 2 nudges) until a lookup succeeded; `contract_met` recorded |
| `funes` | `/api/chat` of a throwaway account on a scratch `FUNES_DB`/`FUNES_DATA_DIR` in a temp dir; real `agents/funes.yaml`, auto-memory/consolidation/backfill off; sample's triples + `--funes-distractors` (default 2000) others in the pool, loaded through `POST /api/memories`. Repo `data/memory.db` mtime is checked before/after. |
| `web` | `web_search` (Parallel's keyless MCP first, Tavily fallback, as Funes; backend per call in `search_backends`; no synthesised answer) + `web_fetch`; taint = any web tool call |
| `web+cache` | `web` + `recall`/`remember` over notes the model wrote this run; two passes over the same questions, `pass2` is the H6 number; an answer that used a recalled note is tainted |

The store is the **whole** PopQA triple set (14,267), not only the sample, so
same-name entities and same-relation neighbours are real distractors. The
`funes` pool is smaller (sample + 2000), so `funes` vs `store` is not yet a
one-variable comparison — close that before the paper (open problem in the
notebook).

## Scoring and cost

Primary: `contains` — a gold alias occurs in the answer as whole words after
normalisation (PopQA's own metric). Also `exact` (whole answer equals an
alias) and an `abstained` flag. Cost per answer = prompt + completion tokens
and wall ms, summed over the tool rounds, from llama-server's `timings`.
`funes` records wall time only: the SSE reports context fill, not tokens.
No LLM judge in E0.

## Results

`results/popqa-n<N>-<model-tag>-<condition>-r<repeat>.json`: model file, quant,
endpoint, n_ctx (from the server, at run start), git commit + dirty flag, dataset
sha256 and sample seed, decoding seed, date, store size, then every answer
(prediction, scores, usage, tools called, nudges, taint) and a summary. `*smoke*`
files are plumbing checks and are gitignored.

## Added after the E0 design review (2026-11-15)

- **Raw web payloads** go to `results/payloads/<tag>/<condition>-r<rep>-p<pass>/<question id>.json.gz` (gitignored): every search attempt's raw reply (Parallel and Tavily) and every fetched page, so web cells can be re-scored.
- **`hedge`**: the answer names >= 2 distinct objects of the same (subject, property) set. `summarize` prints `acc-hedge` (`contains` and not hedged) and the hedge rate; it computes both from stored predictions for older cells.
- **`funes` pool**: `--funes-pool full` (default) loads all 14,267 triples, the list `store` is built from, once into `data/pools/` and copies it per run. `--funes-pool distractors` is the E0 plumbing pool (shown as `funes@3000`).
- **`--sample stratified`** (equal items per popularity bucket; give it a new `--dataset-seed`). Names carry `stratified<seed>`.
- **`summarize --by-bucket`**: accuracy per popularity bucket with n.
