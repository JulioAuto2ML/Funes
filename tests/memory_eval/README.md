# memory_eval — recall, measured

Phase 1 of [docs/memory-plan.md](../../docs/memory-plan.md). A report, not a
pass/fail test: run it before and after a change to the memory model and put
the numbers in the commit.

```bash
# a CPU embed server with yoda's flags (any nomic-embed-text-v1.5 gguf)
llama-server -m nomic-embed-text-v1.5.Q4_K_M.gguf --port 18081 --embedding \
  --pooling mean --ctx-size 8192 --batch-size 8192 --ubatch-size 8192 \
  --parallel 1 --override-kv nomic-bert.context_length=int:8192 --device none

./bin/memory_eval --embed-url http://127.0.0.1:18081 --verbose
```

`--no-embed` measures keyword-only recall (what a deployment without an
embedding endpoint gets); `--json` prints per-query detail; `--k` changes the
window (the agent recalls 4 by default).

## The fixture

`fixture.json` is synthetic: invented content in the shapes yoda's pool had on
2026-11-10. Three quarters exchanges, the most-recalled ones meta chats and
attachments, a stale negative ("I don't have your name") beside the fact it
contradicts, a superseded job description still live, and tool memories
copied out of web pages and email. The real pool is personal and stays out of
git.

Each memory is either a fact (`text`, `source`) or an exchange (`user`,
`reply`), which is stored through `funes::auto_memory_text` — the same
function the agent uses — so a change to what exchanges are stored as shows
up here. `tainted_by` marks a memory a run wrote after reading third-party
content; `noise` marks one that should not answer anything.

Each query names its `relevant` memories and its `stale` ones (outdated,
contradicting, or planted).

## Metrics

| | meaning |
|---|---|
| recall@k | share of a query's relevant memories in the top k |
| hit@1 | the first memory is relevant |
| stale-first | a stale memory outranks every relevant one — lower is better |
| contamination@k | share of the k slots holding a stale memory, or noise not relevant to the query — lower is better |

## History

Semantic, k=4, 30 queries, nomic-embed-text-v1.5 Q4_K_M on CPU.

| change | recall@4 | hit@1 | stale-first | contamination@4 |
|---|---|---|---|---|
| baseline (7.0.1) | 0.750 | 0.700 | 0.167 | 0.142 |
