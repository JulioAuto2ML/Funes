#!/bin/bash
# `funes` rows (D1): one labelled "in the real system" line per size, 1 repeat, 1000 unambiguous items, IQ4_XS 0.8/2/4/9/27B.
# Launch on yoda from ~/funes-ko/benchmarks/knowledge-offload:   nohup bash results/queue_funes.sh >/dev/null 2>&1 &
#   DRY=1 prints every command and writes nothing.   DEADLINE="2026-11-19 05:45" default.
# Block caps (minutes) are ESTIMATED, not measured for these sizes: E0's funes cell at 9B Q8_0 took 5168.8 s (86 min) for 1000 items;
# cap = that x 1.25 + swap for 0.8-9B, 27B cap 180 min is a guess. ko_swap --max-minutes kills a block at its cap (the cell is then not written).
# Smoke (n=10, 9B IQ4, pool 14267) passed 2026-11-18 14:0x. No web: no Parallel, no Tavily.
# Log: results/queue_funes.log. Verify afterwards: ls results/*-funes-*.json (5 files) and python3 run.py summarize --glob "popqa-stratified1u-n1000-*funes*.json"
DEADLINE=${DEADLINE:-"2026-11-19 05:45"}
DRY=${DRY:-0}
cd ~/funes-ko/benchmarks/knowledge-offload 2>/dev/null || cd "$(dirname "$0")/.." || exit 1
log(){ if [ "$DRY" = 1 ]; then echo "[dry] $*"; else echo "$(date +%FT%T) $*" >> results/queue_funes.log; fi; }
SAMPLE="--sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000"
COMMON="--llm-url http://localhost:8080 --embed-url http://localhost:8081 --allow-local --repeats 1 --funes-bin /home/julio/Funes/bin/funes --funes-pool full"
DEADLINE_EPOCH=$(date -d "$DEADLINE" +%s) || exit 1
declare -A MINUTES=( [qwen3.5-9b-iq4_xs]=110 [qwen3.5-4b-iq4_xs]=110 [qwen3.5-2b-iq4_xs]=110 [qwen3.5-0.8b-iq4_xs]=110 [qwen3.5-27b-iq4_xs]=180 )
for ID in qwen3.5-9b-iq4_xs qwen3.5-4b-iq4_xs qwen3.5-2b-iq4_xs qwen3.5-0.8b-iq4_xs qwen3.5-27b-iq4_xs; do
  MIN=${MINUTES[$ID]}
  RUN=results/run-funes-$ID.sh
  BODY="#!/bin/bash
cd ~/funes-ko/benchmarks/knowledge-offload
python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --conditions funes >> results/run-funes-$ID.log 2>&1"
  if [ "$DRY" = 1 ]; then echo "--- $ID (cap $MIN min)"; echo "$BODY"; echo "python3 ../../scripts/ko_swap.py run $ID --max-minutes $MIN -- $RUN"; continue; fi
  printf '%s\n' "$BODY" > $RUN; chmod +x $RUN
  for try in 1 2 3 4 5 6; do
    if [ $(( $(date +%s) + MIN * 60 )) -gt $DEADLINE_EPOCH ]; then log "$ID skipped: cap $MIN min does not fit before $DEADLINE"; break; fi
    log "start $ID try $try (max $MIN min)"
    python3 ../../scripts/ko_swap.py run $ID --max-minutes $MIN -- $RUN >> results/ko_swap-funes-$ID.log 2>&1; rc=$?
    log "$ID rc=$rc"
    if [ $rc -eq 0 ]; then break; fi
    if [ $rc -eq 75 ] || grep -q "refusing" results/ko_swap-funes-$ID.log; then sleep 1200; continue; fi
    log "$ID hard failure rc=$rc, queue stops"; exit $rc
  done
done
log "QUEUE DONE"
