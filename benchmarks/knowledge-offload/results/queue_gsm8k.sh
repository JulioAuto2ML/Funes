#!/bin/bash
# GSM8K arm of H2 (design NOTEBOOK "H2 / GSM8K arm", julio's answers 2026-11-18: stages 1, 2 and 3 yes, 27B in the window).
# Stage 1: IQ4_XS 0.8/2/4/9/27B x (closed, store) | Stage 2: Q8_0 0.8/2/4/9B x (closed, store) | Stage 3: IQ4_XS x tool (exploratory, outside H2).
# Smallest first, one swap per model. n = 500, one repeat, max_tokens MAXTOK (512 unless the pilot rule changed it for every cell).
# Launch on yoda from ~/funes-ko/benchmarks/knowledge-offload, AFTER the pilot was read:
#   STAGES="1 2 3" MAXTOK=512 nohup bash results/queue_gsm8k.sh >/dev/null 2>&1 &
#   DRY=1 prints every command and writes nothing.   DEADLINE="2026-11-20 05:45" default.
# Block caps (minutes) are ESTIMATED: design §9 rates (measured on PopQA cells) x an ASSUMED 250 completion tokens, x2 for the
# worst case of truncation at 512. The pilot's measured tokens replace the assumption; edit the numbers then, in a commit.
# Exit codes of a block: 0 done, 75 a cron window stopped it (retried after 20 min), anything else stops the queue.
# Log: results/queue_gsm8k.log. Verify afterwards: python3 verify_gsm8k.py
DEADLINE=${DEADLINE:-"2026-11-20 05:45"}
STAGES=${STAGES:-"1 2"}
MAXTOK=${MAXTOK:-512}
DRY=${DRY:-0}
cd ~/funes-ko/benchmarks/knowledge-offload 2>/dev/null || cd "$(dirname "$0")/.." || exit 1
log(){ if [ "$DRY" = 1 ]; then echo "[dry] $*"; else echo "$(date +%FT%T) $*" >> results/queue_gsm8k.log; fi; }
DEADLINE_EPOCH=$(date -d "$DEADLINE" +%s) || exit 1
# "<id> <conditions> <cap minutes>"
BLOCKS=()
for S in $STAGES; do
  case $S in
    1) BLOCKS+=("qwen3.5-0.8b-iq4_xs closed,store 80" "qwen3.5-2b-iq4_xs closed,store 80" "qwen3.5-4b-iq4_xs closed,store 125"
                "qwen3.5-9b-iq4_xs closed,store 170" "qwen3.5-27b-iq4_xs closed,store 460");;
    2) BLOCKS+=("qwen3.5-0.8b-q8_0 closed,store 80" "qwen3.5-2b-q8_0 closed,store 90" "qwen3.5-4b-q8_0 closed,store 165"
                "qwen3.5-9b-q8_0 closed,store 255");;
    3) BLOCKS+=("qwen3.5-0.8b-iq4_xs tool 60" "qwen3.5-2b-iq4_xs tool 60" "qwen3.5-4b-iq4_xs tool 95"
                "qwen3.5-9b-iq4_xs tool 130" "qwen3.5-27b-iq4_xs tool 340");;
  esac
done
for B in "${BLOCKS[@]}"; do
  read ID CONDS MIN <<< "$B"
  TAGNAME=${ID}-${CONDS//,/_}
  RUN=results/run-gsm8k-$TAGNAME.sh
  BODY="#!/bin/bash
cd ~/funes-ko/benchmarks/knowledge-offload
python3 -u run.py run --task gsm8k --model-tag $ID --expect $ID --llm-url http://localhost:8080 --allow-local --repeats 1 --max-tokens $MAXTOK --conditions ${CONDS//,/ } >> results/run-gsm8k-$TAGNAME.log 2>&1"
  if [ "$DRY" = 1 ]; then echo "--- $ID $CONDS (cap $MIN min)"; echo "$BODY"; echo "python3 ../../scripts/ko_swap.py run $ID --max-minutes $MIN -- $RUN"; continue; fi
  printf '%s\n' "$BODY" > $RUN; chmod +x $RUN
  for try in 1 2 3 4 5 6; do
    if [ $(( $(date +%s) + MIN * 60 )) -gt $DEADLINE_EPOCH ]; then log "$ID $CONDS skipped: cap $MIN min does not fit before $DEADLINE"; break; fi
    log "start $ID $CONDS try $try (max $MIN min)"
    python3 ../../scripts/ko_swap.py run $ID --max-minutes $MIN -- $RUN >> results/ko_swap-gsm8k-$TAGNAME.log 2>&1; rc=$?
    log "$ID $CONDS rc=$rc"
    if [ $rc -eq 0 ]; then break; fi
    if [ $rc -eq 75 ] || grep -q "refusing" results/ko_swap-gsm8k-$TAGNAME.log; then sleep 1200; continue; fi
    log "$ID $CONDS hard failure rc=$rc, queue stops"; exit $rc
  done
done
log "QUEUE DONE"
