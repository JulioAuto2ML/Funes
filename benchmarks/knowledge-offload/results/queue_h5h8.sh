#!/bin/bash
# D12/D13/D15(1): `tool` and `tool+contract` with the DEFAULT prompt (no search_first) on the E1 sample, plus the ablated arm
# for the same two conditions on the default tool prompt (variant abstain_default). No web: no Parallel, no Tavily.
# 9 model files x (2 main cells of 1000 + 2 ablated cells of 200) = 36 cells. Each cell is resumable (run.py skips a finished one).
# Launch on yoda from ~/funes-ko/benchmarks/knowledge-offload:   nohup bash results/queue_h5h8.sh >/dev/null 2>&1 &
#   DRY=1 bash results/queue_h5h8.sh      prints every command, runs and writes nothing
#   DEADLINE="2026-11-18 05:45"           default; no block is started that its upper-bound estimate says would end later
# Log: results/queue_h5h8.log. Exit codes of a block: 0 done, 75 a cron window stopped it (retried after 20 min), anything else stops the queue.
# Verify afterwards: python3 verify_h5h8.py
DEADLINE=${DEADLINE:-"2026-11-18 05:45"}
DRY=${DRY:-0}
cd ~/funes-ko/benchmarks/knowledge-offload 2>/dev/null || cd "$(dirname "$0")/.." || exit 1
log(){ if [ "$DRY" = 1 ]; then echo "[dry] $*"; else echo "$(date +%FT%T) $*" >> results/queue_h5h8.log; fi; }
grep -q abstain_default ko/conditions.py || { echo "this checkout has no abstain_default variant: update it first"; exit 1; }
SAMPLE="--sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000"
COMMON="--llm-url http://localhost:8080 --allow-local --repeats 1"
DEADLINE_EPOCH=$(date -d "$DEADLINE" +%s) || exit 1
# models in the order of value: IQ4_XS sizes (H8 slope, H5), 27B last of them (longest), then the Q8_0 control
for ID in qwen3.5-9b-iq4_xs qwen3.5-4b-iq4_xs qwen3.5-2b-iq4_xs qwen3.5-0.8b-iq4_xs qwen3.5-27b-iq4_xs \
          qwen3.5-9b-q8_0 qwen3.5-4b-q8_0 qwen3.5-2b-q8_0 qwen3.5-0.8b-q8_0; do
  MIN=$(python3 est_h5h8.py minutes $ID 2>/dev/null || echo 180)
  RUN=results/run-h5h8-$ID.sh
  BODY="#!/bin/bash
cd ~/funes-ko/benchmarks/knowledge-offload
python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --prompt-variant default --conditions tool tool+contract >> results/run-h5h8-$ID.log 2>&1
r=\$?; echo \"main exit \$r\" >> results/run-h5h8-$ID.log; [ \$r -eq 0 ] || exit \$r
python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --ablated --prompt-variant abstain_default --conditions tool tool+contract >> results/run-h5h8-$ID-ablated.log 2>&1
r2=\$?; echo \"ablated exit \$r2\" >> results/run-h5h8-$ID-ablated.log; exit \$r2"
  if [ "$DRY" = 1 ]; then
    echo "--- $ID (upper-bound block estimate $MIN min)"; echo "$BODY"
    echo "python3 ../../scripts/ko_swap.py run $ID --max-minutes $MIN -- $RUN"
    continue
  fi
  printf '%s\n' "$BODY" > $RUN; chmod +x $RUN
  for try in 1 2 3 4 5 6; do
    if [ $(( $(date +%s) + MIN * 60 )) -gt $DEADLINE_EPOCH ]; then log "$ID skipped: upper estimate $MIN min does not fit before $DEADLINE"; break; fi
    log "start $ID try $try (max $MIN min)"
    python3 ../../scripts/ko_swap.py run $ID --max-minutes $MIN -- $RUN >> results/ko_swap-h5h8-$ID.log 2>&1; rc=$?
    log "$ID rc=$rc"
    if [ $rc -eq 0 ]; then break; fi
    if [ $rc -eq 75 ] || grep -q "refusing" results/ko_swap-h5h8-$ID.log; then sleep 1200; continue; fi
    log "$ID hard failure rc=$rc, queue stops"; exit $rc
  done
done
log "QUEUE DONE"
