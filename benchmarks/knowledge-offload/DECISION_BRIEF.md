# Pre-E1 design decisions — brief for a Fable session

Read first: `docs/knowledge-offload-plan.md` ("The question", "E0", "E1", "E2", "The paper → Hypotheses, Statistics", "Open questions"), `benchmarks/knowledge-offload/NOTEBOOK.md` (Now block and the last entries), then `results/popqa-n1000-qwen3.8-9b-q8-*-r{0,1,2}.json` via `python3 run.py summarize` (no pasting raw JSON). Do not edit code. Do not run models.

Decide, with reasons and what would change your mind, and write the outcome as a dated entry in `NOTEBOOK.md` plus a dated line in the plan:

1. **Does `funes` run in every E1 cell, or only as one final "in the real system" row?** Evidence: ~85 min per repeat against 5-40 min for the others; verbose answers; wall time only, no tokens; repeat-to-repeat spread of `funes` against the other conditions (from E0, 3 repeats); what H1-H3 and H5 need from it.
2. **The pool mismatch.** `funes` pool = sample + 2000 distractors; `store` = all 14,267 triples. Choose: load all into `funes`, shrink `store`, or accept and label it. Say which comparison each choice breaks or protects ("one variable per comparison").
3. **H6 and taint.** A `web+cache` answer that used a recalled note counts as tainted. Does that definition make H6 testable as written, and is the `web` backend mix (Parallel first, Tavily fallback, recorded per call) a confound for the paper?
4. **Anything in E0's results that contradicts a hypothesis or exposes a flaw in the metric** (`contains` primary, `exact` kept; the `closed` score; `store` retrieval hit rate near 1.0 on a store containing the answer, i.e. is E1's kill criterion even reachable on this sample?). By the plan's own rule, a surprising result is the trigger to escalate here.

Output: at most 40 lines, decisions first. Say what Opus 5.5 · high should do next and what stays Sonnet 5.5 · medium.
