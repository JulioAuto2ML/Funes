#!/bin/bash
# Runs the remaining E1 sizes back to back; resumes after a cron window (exit 75/refusal). Log: results/queue.log
cd ~/funes-ko/benchmarks/knowledge-offload
log(){ echo "$(date +%FT%T) $*" >> results/queue.log; }
SAMPLE="--sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000"
COMMON="--llm-url http://localhost:8080 --allow-local --repeats 1"
while pgrep -f "ko_swap.py run qwen3.5-9b-iq4_xs" >/dev/null; do sleep 15; done
log "9b block finished; queue starts"
# id | extra conditions (web only at 0.8B and 27B)
for job in "qwen3.5-4b-iq4_xs|" "qwen3.5-2b-iq4_xs|" "qwen3.5-0.8b-iq4_xs|web" "qwen3.5-27b-iq4_xs|web" \
           "qwen3.5-9b-q8_0|" "qwen3.5-4b-q8_0|" "qwen3.5-2b-q8_0|" "qwen3.5-0.8b-q8_0|"; do
  ID=${job%%|*}; EXTRA=${job##*|}
  cat > results/run-$ID.sh <<EOF
#!/bin/bash
cd ~/funes-ko/benchmarks/knowledge-offload
python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --prompt-variant search_first --conditions closed oracle store tool tool+contract $EXTRA >> results/run-$ID.log 2>&1
r=\$?; echo "main exit \$r" >> results/run-$ID.log; [ \$r -eq 0 ] || [ \$r -eq 3 ] || exit \$r
python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --ablated --prompt-variant abstain_ok >> results/run-$ID-ablated.log 2>&1
r2=\$?; echo "ablated exit \$r2" >> results/run-$ID-ablated.log; [ \$r -eq 3 ] && exit 3; exit \$r2
EOF
  chmod +x results/run-$ID.sh
  for try in 1 2 3 4 5 6; do
    log "start $ID try $try"
    python3 ../../scripts/ko_swap.py run $ID -- results/run-$ID.sh >> results/ko_swap-$ID.log 2>&1; rc=$?
    log "$ID rc=$rc"
    if [ $rc -eq 0 ]; then break; fi
    if [ $rc -eq 3 ]; then log "$ID web 429-blocked, moving on"; break; fi
    if [ $rc -eq 75 ] || grep -q "refusing" results/ko_swap-$ID.log; then sleep 1200; continue; fi
    log "$ID hard failure rc=$rc, queue stops"; exit $rc
  done
done
log "QUEUE DONE"
