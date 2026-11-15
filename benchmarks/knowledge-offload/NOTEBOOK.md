# Lab notebook — knowledge offload

Plan: `docs/knowledge-offload-plan.md`. A new session reads the plan section it
needs, this "Now" block and the last three entries, nothing else.

## Now
- **Step:** 1, E0 harness. Code done and smoke-tested; **the E0 run itself has not happened.**
- **Blocker:** yoda :8080 serves Qwen3.8-27B (production). `Qwen3.8-9B-Q8_0.gguf` is on disk, not loaded. A swap is julio's call (log it, undo it, not during the crons in `scripts/yoda.crontab`). Web conditions wait for the six-condition run (pid 2609426) to finish.
- **Next action (web, after the six-condition run ends, 9B still on :8080):** the command in the 2026-11-15 web entry. (Older note: julio swaps :8080 to the 9B, then `python3 -u run.py run --model-tag qwen3.8-9b-q8 --expect-model 9B > results/run.log 2>&1`, then restore `qwen27b`.)
- **Claude needed:** none for the run (script). Sonnet 5.5 · medium to fix what it shows.

## 2026-11-15 · step 1 · E0 harness · Sonnet 5.5 · medium
- Built `benchmarks/knowledge-offload/`: `run.py` + `ko/` (8 conditions, scorer, FTS5 triple store, llama-server client with timings, scratch-Funes driver, stats), 16 hermetic tests pass, README. PopQA `test.tsv` fetched (sha256 in results).
- Smoke on the 27B, n=20, 1 repeat (`results/*smoke*`, gitignored, plumbing only): closed .25, oracle/store/tool 1.0, store retrieval hit 1.0. n=4 funes/web/web+cache ran end to end; web+cache pass 2 made no web calls.
- Decisions (also in the plan): `contains` primary, `exact` kept; 1000 items, dataset seed 0; store = all 14,267 triples, BM25; `--expect-model` refuses a run on the wrong model; pip can't reach PyPI here, so the venv has no pytest (harness is stdlib).
- Decision: a `web+cache` answer that used a recalled note counts as tainted (H6).
- Open: (1) no 9B served, so no E0 numbers, no repeat-noise check yet. (2) `funes` pool = sample + 2000 distractors vs `store` = 14,267: not one-variable; load all or shrink `store` before E1. (3) `funes` answers are verbose and ignore `lookup`-style tools (recall is auto-injected); `funes` cost is wall-only (SSE has no tokens). (4) Web budget: web ×3 repeats + web+cache ×2 passes ×3 ≈ 5-10k Tavily searches for 1k questions; check the plan/quota before running them; the key is read from `config/funes.local`, never stored in results. (5) Qwen3.8's thinking is off via `enable_thinking:false`; confirm the 9B honours it.
- Next: swap to 9B (julio), run E0 3 repeats, `summarize`, check spread, write the first table here. Claude: none to run; Sonnet 5.5 · medium after.

## 2026-11-15 · step 1 · web backend · Sonnet 5.5 · medium
- Added Parallel as the first `web_search` backend in `ko/tools.py` (MCP streamable HTTP, keyless, session reused), Tavily as fallback, as Funes does (`FUNES_SEARCH_BACKEND=parallel`; `FUNES_PARALLEL_API_KEY` optional). Every search is logged per answer in `search_backends` (`parallel` | `tavily` + `fallback_reason` | `none`) in the result JSON; `search_backend` config is in the cell's meta. 4 new hermetic tests, 20 pass.
- Live probe, 8 sequential searches: all answered by Parallel, 0.8-2.1 s each, no 429 and no rate-limit headers. Real limit at volume unknown: watch `search_backends` for `tavily` in the first cells. Parallel hits are ~3.5 kB each (5 excerpts) vs Tavily's 400-char snippets, so web prompts are larger than the smoke numbers; compare cost per answer with that in mind.
- Open: if Parallel rate-limits mid-run and Tavily's 1,000/month runs out, `search` raises and the answer is recorded as an error (not a guess); rerun resumes cells but not partial cells.
- Exact command for the web run (do NOT start while the six-condition run is going; same 9B, same tag, cells are resumable):
  `cd benchmarks/knowledge-offload && python3 -u run.py run --model-tag qwen3.8-9b-q8 --expect-model 9B --conditions web web+cache > results/run-web.log 2>&1`
- Next: after the run, `python3 run.py summarize`; count `search_backends` by backend. Claude: none to run.
