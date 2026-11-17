#!/bin/bash
cd ~/funes-ko/benchmarks/knowledge-offload
python3 -u run.py run --model-tag qwen3.5-4b-iq4_xs --expect qwen3.5-4b-iq4_xs --sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000 --llm-url http://localhost:8080 --allow-local --repeats 1 --prompt-variant default --conditions tool tool+contract >> results/run-h5h8-qwen3.5-4b-iq4_xs.log 2>&1
r=$?; echo "main exit $r" >> results/run-h5h8-qwen3.5-4b-iq4_xs.log; [ $r -eq 0 ] || exit $r
python3 -u run.py run --model-tag qwen3.5-4b-iq4_xs --expect qwen3.5-4b-iq4_xs --sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000 --llm-url http://localhost:8080 --allow-local --repeats 1 --ablated --prompt-variant abstain_default --conditions tool tool+contract >> results/run-h5h8-qwen3.5-4b-iq4_xs-ablated.log 2>&1
r2=$?; echo "ablated exit $r2" >> results/run-h5h8-qwen3.5-4b-iq4_xs-ablated.log; exit $r2
