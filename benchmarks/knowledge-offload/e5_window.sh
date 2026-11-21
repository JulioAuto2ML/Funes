#!/bin/bash
# E5 eval window (E5_DESIGN section 2). Run inside `ko_swap run qwen3.5-2b-iq4_xs --max-minutes N -- bash e5_window.sh`
# (27B stopped, a plain 2B IQ4_XS on :8080). For each (base, adapter) the plan needs: a plain base server on :8198 and the same
# file with `--lora ... --lora-init-without-apply` on :8199; gate 3(b) (scale 0 identical to base, scale 1 differs) unless the
# row is the Q8_0 transfer; scale 1; the cells of `e5_eval.py commands` for that server. Cells that exist are skipped (resumable).
cd ~/funes-ko/benchmarks/knowledge-offload || exit 2
KO=data/e5; OUT=$KO/eval; mkdir -p $OUT; PY=python3
MD=~/llama.cpp/models; SRV=~/llama.cpp/bin/llama-server
serve() {  # port, extra args...
  P=$1; shift
  $SRV -m "$BASE_FILE" "$@" --port $P --host 127.0.0.1 -t 8 -ngl 99 -fa on --ctx-size 8192 --parallel 1 --cache-ram 0 --jinja \
     --reasoning-budget 0 --chat-template-kwargs '{"enable_thinking":false}' > $OUT/server_$P.log 2>&1 &
  echo $!
}
wait_up() { for i in $(seq 1 90); do curl -s -m 2 localhost:$1/health | grep -q ok && return 0; sleep 2; done; return 1; }
$PY e5_eval.py servers | while read BASE ADAPTER; do
  echo "=== $BASE + $ADAPTER $(date +%T)"
  BASE_FILE=$MD/$($PY e5_eval.py file --base $BASE); AD=$MD/adapters/$($PY e5_eval.py file --adapter $ADAPTER)
  case $BASE in *0.8b*) DEV=$KO/build_anchor_0.8b/dev_items.jsonl;; *) DEV=$KO/build_anchor_2b/dev_items.jsonl;; esac
  SB=""; SL=$(serve 8199 --lora $AD --lora-init-without-apply); wait_up 8199 || { echo "lora server did not start"; kill $SL; continue; }
  if [ "${BASE#*q8_0}" = "$BASE" ]; then
    SB=$(serve 8198); wait_up 8198 || { echo "base server did not start"; kill $SL $SB; continue; }
    $PY -I e5_lora_gate.py --base-url http://localhost:8198 --lora-url http://localhost:8199 --dev $DEV --n 20 --allow-local --out $OUT/gate_$ADAPTER.json < /dev/null
    G=$?; echo "gate $ADAPTER rc=$G"
    kill $SB; wait $SB 2>/dev/null
    [ $G = 0 ] || { kill $SL; wait $SL 2>/dev/null; continue; }
  else echo "gate skipped: $ADAPTER on $BASE is the transfer row (gate ran on its own base)"; fi
  curl -s -X POST localhost:8199/lora-adapters -H "Content-Type: application/json" -d '[{"id":0,"scale":1.0}]' > /dev/null < /dev/null
  $PY e5_eval.py commands --base $BASE --adapter $ADAPTER | while read -r CMD; do
    echo "--- $(date +%T) $CMD"
    $PY -u $CMD --llm-url http://localhost:8199 --allow-local < /dev/null; echo "rc=$?"
  done
  kill $SL; wait $SL 2>/dev/null
done
echo "=== window done $(date +%T)"; $PY verify_e5.py | tail -30
