#!/bin/bash
cd ~/funes-ko/benchmarks/knowledge-offload
python3 -u run.py run --model-tag qwen3.5-9b-q8_0 --expect qwen3.5-9b-q8_0 --sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000 --llm-url http://localhost:8080 --allow-local --repeats 1 --prompt-variant search_first --conditions closed oracle store tool tool+contract  >> results/run-qwen3.5-9b-q8_0.log 2>&1
r=$?; echo "main exit $r" >> results/run-qwen3.5-9b-q8_0.log; [ $r -eq 0 ] || [ $r -eq 3 ] || exit $r
python3 -u run.py run --model-tag qwen3.5-9b-q8_0 --expect qwen3.5-9b-q8_0 --sample stratified --exclude-ambiguous --dataset-seed 1 --n 1000 --llm-url http://localhost:8080 --allow-local --repeats 1 --ablated --prompt-variant abstain_ok >> results/run-qwen3.5-9b-q8_0-ablated.log 2>&1
r2=$?; echo "ablated exit $r2" >> results/run-qwen3.5-9b-q8_0-ablated.log; [ $r -eq 3 ] && exit 3; exit $r2
