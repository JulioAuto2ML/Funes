#!/bin/bash
# GSM8K pilot (design §5, §9): 20 problems never in the n=500 sample, `closed`, 0.8B and 9B IQ4_XS. Two jobs: (a) share of answers that
# end at the token limit on the 9B (> 10% -> max_tokens 1024 for every cell), (b) mean completion tokens, which replaces the
# 250-token ASSUMPTION of the time estimate. Pilot files are gsm8k-pilot-* (never committed, never analysed beyond this).
# Launch on yoda from ~/funes-ko/benchmarks/knowledge-offload:   bash results/pilot_gsm8k.sh   (DRY=1 prints only)
DRY=${DRY:-0}
cd ~/funes-ko/benchmarks/knowledge-offload 2>/dev/null || cd "$(dirname "$0")/.." || exit 1
for ID in qwen3.5-9b-iq4_xs qwen3.5-0.8b-iq4_xs; do
  RUN=results/run-gsm8k-pilot-$ID.sh
  BODY="#!/bin/bash
cd ~/funes-ko/benchmarks/knowledge-offload
python3 -u run.py run --task gsm8k --pilot --model-tag $ID --expect $ID --llm-url http://localhost:8080 --allow-local --repeats 1 --conditions closed >> results/run-gsm8k-pilot-$ID.log 2>&1"
  if [ "$DRY" = 1 ]; then echo "$BODY"; echo "python3 ../../scripts/ko_swap.py run $ID --max-minutes 25 -- $RUN"; continue; fi
  printf '%s\n' "$BODY" > $RUN; chmod +x $RUN
  python3 ../../scripts/ko_swap.py run $ID --max-minutes 25 -- $RUN >> results/ko_swap-gsm8k-pilot-$ID.log 2>&1; echo "$(date +%FT%T) $ID rc=$?" >> results/pilot_gsm8k.log
done
