#!/bin/bash
cd ~/funes-ko/benchmarks/knowledge-offload
ID=qwen3.5-9b-iq4_xs
SAMPLE="--sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000"
COMMON="--llm-url http://localhost:8080 --allow-local --repeats 1"
python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --prompt-variant search_first --conditions closed oracle store tool tool+contract web web+writeback > results/run-$ID.log 2>&1
echo "main exit $?" >> results/run-$ID.log
python3 -u run.py run --model-tag $ID --expect $ID $SAMPLE $COMMON --ablated --prompt-variant abstain_ok > results/run-$ID-ablated.log 2>&1
echo "ablated exit $?" >> results/run-$ID-ablated.log
