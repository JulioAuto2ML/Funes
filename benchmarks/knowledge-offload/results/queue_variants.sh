#!/bin/bash
# Stages A and B of the default-prompt variants (notebook "step 2 of the 9B plan", approved by julio 2026-11-17):
# condition `tool` only, variants tool_first / tool_free / tool_describe, 1000 unambiguous items, 4 models x 3 = 12 cells.
# A: qwen3.5-9b-iq4_xs, qwen3.5-4b-iq4_xs.  B: qwen3.5-4b-q8_0, qwen3.5-9b-q8_0.  No web: no Parallel, no Tavily.
# Launch on yoda from ~/funes-ko/benchmarks/knowledge-offload:   nohup bash results/queue_variants.sh >/dev/null 2>&1 &
#   DRY=1 bash results/queue_variants.sh      prints every command, runs and writes nothing
#   DEADLINE="2026-11-18 05:45"               default; no block is started that its upper-bound estimate says would end later
# Log: results/queue_variants.log. Exit codes of a block: 0 done, 75 a cron window stopped it (retried after 20 min), anything else stops the queue.
# Block upper bound (minutes) = (3 x the measured wall_s of that size's `tool/search_first` cell + 149 s swap, the largest measured) x 1.25:
#   9b-iq4 973.9 s -> 65, 4b-iq4 766.1 s -> 52, 4b-q8 1051.0 s -> 70, 9b-q8 1622.8 s -> 105.
# search_first is the high end because it looks up on ~every question; how many calls each new wording makes is NOT measured.
# Verify afterwards: python3 verify_variants.py
DEADLINE=${DEADLINE:-"2026-11-18 05:45"}
DRY=${DRY:-0}
cd ~/funes-ko/benchmarks/knowledge-offload 2>/dev/null || cd "$(dirname "$0")/.." || exit 1
log(){ if [ "$DRY" = 1 ]; then echo "[dry] $*"; else echo "$(date +%FT%T) $*" >> results/queue_variants.log; fi; }
grep -q tool_describe ko/conditions.py || { echo "this checkout has no tool_describe variant: update it first"; exit 1; }
SAMPLE="--sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000"
COMMON="--llm-url http://localhost:8080 --allow-local --repeats 1"
DEADLINE_EPOCH=$(date -d "$DEADLINE" +%s) || exit 1
declare -A MINUTES=( [qwen3.5-9b-iq4_xs]=65 [qwen3.5-4b-iq4_xs]=52 [qwen3.5-4b-q8_0]=70 [qwen3.5-9b-q8_0]=105 )
for ID in qwen3.5-9b-iq4_xs qwen3.5-4b-iq4_xs qwen3.5-4b-q8_0 qwen3.5-9b-q8_0; do
  MIN=${MINUTES[$ID]}
  RUN=results/run-variants-$ID.sh
  BODY="#!/bin/bash
cd ~/funes-ko/benchmarks/knowledge-offload
rc=0
for V in tool_first tool_free tool_describe; do
  python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --prompt-variant \$V --conditions tool >> results/run-variants-$ID.log 2>&1
  r=\$?; echo \"\$V exit \$r\" >> results/run-variants-$ID.log; [ \$r -eq 0 ] || { rc=\$r; break; }
done
exit \$rc"
  if [ "$DRY" = 1 ]; then
    echo "--- $ID (upper-bound block estimate $MIN min)"; echo "$BODY"
    echo "python3 ../../scripts/ko_swap.py run $ID --max-minutes $MIN -- $RUN"
    continue
  fi
  printf '%s\n' "$BODY" > $RUN; chmod +x $RUN
  for try in 1 2 3 4 5 6; do
    if [ $(( $(date +%s) + MIN * 60 )) -gt $DEADLINE_EPOCH ]; then log "$ID skipped: upper estimate $MIN min does not fit before $DEADLINE"; break; fi
    log "start $ID try $try (max $MIN min)"
    python3 ../../scripts/ko_swap.py run $ID --max-minutes $MIN -- $RUN >> results/ko_swap-variants-$ID.log 2>&1; rc=$?
    log "$ID rc=$rc"
    if [ $rc -eq 0 ]; then break; fi
    if [ $rc -eq 75 ] || grep -q "refusing" results/ko_swap-variants-$ID.log; then sleep 1200; continue; fi
    log "$ID hard failure rc=$rc, queue stops"; exit $rc
  done
done
log "QUEUE DONE"
