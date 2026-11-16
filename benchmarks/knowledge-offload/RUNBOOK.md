# E1 runbook — one model size, the full condition set

For julio (or a session julio started) on yoda. Everything here is a GPU job: the swap stops the
production 27B, so it runs only outside Funes's cron windows (`scripts/ko_swap.py status` says when the
next one opens) and **always puts qwen27b back**, on success, failure, ^C, or when a window is about to
open (exit 75; the harness resumes, a partially done cell is redone). Plan: `docs/knowledge-offload-plan.md`,
"E1 pre-registration".

## Once per session

```bash
cd ~/funes-ko && git fetch origin && git checkout --detach origin/feat/knowledge-offload-e0
cd benchmarks/knowledge-offload
python3 -m pytest -q tests                      # hermetic, no model
python3 ../../scripts/ko_swap.py status         # health + when the next cron window opens
export SAMPLE="--sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000"
export COMMON="--llm-url http://localhost:8080 --allow-local --repeats 1"
```

Models: the ids are `models.lock` rows (`qwen3.5-{0.8,2,4,9,27}b-iq4_xs`, control `qwen3.5-{0.8,2,4,9}b-q8_0`).
`ko_swap.py` and `run.py` both refuse a file whose sha256 is not the locked one. Search is Parallel only
(`FUNES_PARALLEL_API_KEY` in `config/funes.local`); Tavily is never used.

## 1. Load test (once, before the 27B row)

Qwen3.5-27B IQ4_XS is 14.98 GB on a 16 GB card. This loads it with the benchmark flags, sends a ~6k-token
prompt, and if f16 KV does not fit retries with q4_0 and writes that decision to `~/.ko_kv`, which
**every** size then uses (one flag set for the whole curve).

```bash
python3 ../../scripts/ko_swap.py loadtest qwen3.5-27b-iq4_xs
cat ~/.ko_kv ; tail -5 ~/llama_logs/model_swaps.log
```

If it prints "does not fit with either KV type", the 27B point needs a smaller context or another quant:
stop and ask; do not change flags for one size only.

## 2. Pilot: Qwen3.5-9B IQ4_XS, 100-item identity check first

Two repeats of three conditions on 100 questions; they must be bit-identical before any repeat count
is trusted (E0's were). `--n 100` takes the first 100 of the same stratified draw, so the tag is separate.

```bash
python3 ../../scripts/ko_swap.py run qwen3.5-9b-iq4_xs --max-minutes 40 -- \
  python3 -u run.py run --model-tag qwen3.5-9b-iq4_xs-pilot --expect qwen3.5-9b-iq4_xs \
    --sample stratified --exclude-ambiguous --dataset-seed 1 --n 100 --repeats 2 \
    --llm-url http://localhost:8080 --allow-local --prompt-variant search_first \
    --conditions closed store tool+contract > results/pilot-identity.log 2>&1
for c in closed store tool+contract-search_first; do
  python3 identity_check.py results/popqa-stratified1u-n100-qwen3.5-9b-iq4_xs-pilot-$(echo $c | sed 's/+/plus/')-r0.json \
                            results/popqa-stratified1u-n100-qwen3.5-9b-iq4_xs-pilot-$(echo $c | sed 's/+/plus/')-r1.json
done
```

Exit 1 from `identity_check.py` means the repeats differ: count them, do not assume one repeat is enough.

## 3. Full set for one size (1 repeat)

Main arm plus the ablated arm in one swap. `web` only at the smallest size, 9B and 27B; `web+writeback`
at 9B only. Drop those two names from `--conditions` for other sizes.

```bash
ID=qwen3.5-9b-iq4_xs
python3 ../../scripts/ko_swap.py run $ID -- bash -c "
  python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --prompt-variant search_first \
    --conditions closed oracle store tool tool+contract web web+writeback > results/run-$ID.log 2>&1;
  python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --ablated --prompt-variant abstain_ok \
    > results/run-$ID-ablated.log 2>&1"
```

* Exit 3 in `run-$ID.log` ("BLOCKED web … 429"): Parallel rate-limited; the web cell was not written, the
  others were. Rerun the same command another day; finished cells are skipped.
* The `funes` row (one size only, D1) is a separate swap: add `--conditions funes --funes-pool full`
  (first use builds the 14,267-triple pool; this path has only run in hermetic tests so far).
* `web` costs ~4,000 s at 9B; check the 429 count per cell in the result's `search` block.

## 4. After the run

```bash
python3 label_abstain.py dump       # 50 non-correct ablated answers, both regex directions -> results/abstain_labels.tsv
# fill the `human` column (abstained / hallucinated / correct), then:
python3 label_abstain.py score results/abstain_labels.tsv     # exit 1 under 90 % agreement: fix the regex first
python3 stats_e0.py --prefix popqa-stratified1u --tag $ID     # accuracy, paired, buckets, ablated section
python3 run.py summarize --glob "popqa-stratified1u*$ID*.json" --by-bucket     # also prints the web+writeback pass 2 vs pass 1
```

Commit the result JSONs and the table, never `results/payloads/` or `*pilot*`/`*smoke*` files.
If a session dies mid-run: `python3 ../../scripts/ko_swap.py restore` (idempotent).
