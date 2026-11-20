#!/usr/bin/env python3
"""The base model's own greedy answers to GSM8K train questions: the anchors' targets (E5_DESIGN §3).

  ko_swap.py run qwen3.5-2b-iq4_xs -- python3 e5_anchor_answers.py --prompts data/e5/gsm8k_train.jsonl \
      --out data/e5/anchor_answers_2b.jsonl --llm-url http://localhost:8080 --allow-local

Prompt = the GSM8K `closed` eval prompt (`gsm8k.SYSTEM`, no tools), greedy, max_tokens 512, the harness's client.
A seeded sample of lines (`--n`, default 1700: more than the 1,500 anchors of the larger set, since cut-off
answers are dropped later) is answered; rows are appended as they finish and a rerun skips what is on disk.
Output rows: {"idx": <line in the prompts file>, "text", "finish_reason", "completion_tokens"} — the format
`ko/e5_data.build_anchor_rows` reads. No scoring: the answers are kept right or wrong on purpose.
"""
import argparse
import json
import random
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ko import gsm8k  # noqa: E402
from ko.llm import Client, LLMError  # noqa: E402

SAMPLE_SEED = 7
MAX_TOKENS = 512


def pick(n_lines: int, n: int, seed: int = SAMPLE_SEED) -> list[int]:
    if n > n_lines:
        raise ValueError(f"{n} requested from {n_lines} prompts")
    return sorted(random.Random(seed).sample(range(n_lines), n))


def done_ids(out: Path) -> set[int]:
    if not out.exists():
        return set()
    return {json.loads(l)["idx"] for l in out.read_text(encoding="utf-8").splitlines() if l.strip()}


def answer_one(client, question: str, seed: int = 0) -> dict:
    r = client.chat([{"role": "system", "content": gsm8k.SYSTEM}, {"role": "user", "content": question}],
                    seed=seed, max_tokens=MAX_TOKENS)
    return {"text": r.text, "finish_reason": r.finish_reason, "completion_tokens": r.usage.as_dict().get("completion_tokens")}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--prompts", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--n", type=int, default=1700)
    ap.add_argument("--llm-url", default="http://localhost:8080")
    ap.add_argument("--allow-local", action="store_true")
    a = ap.parse_args(argv)
    prompts = [json.loads(l) for l in Path(a.prompts).read_text(encoding="utf-8").splitlines() if l.strip()]
    out = Path(a.out)
    todo = [i for i in pick(len(prompts), a.n) if i not in done_ids(out)]
    client = Client(a.llm_url, allow_local=a.allow_local)
    print(f"served: {client.server_info()}", flush=True)
    t0, n_ok = time.monotonic(), 0
    with out.open("a", encoding="utf-8") as f:
        for k, i in enumerate(todo):
            try:
                row = {"idx": i, **answer_one(client, prompts[i]["question"])}
            except LLMError as e:
                print(f"idx {i}: {e}", file=sys.stderr, flush=True)
                continue
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
            f.flush()
            n_ok += 1
            if (k + 1) % 100 == 0:
                print(f"  {k + 1}/{len(todo)}  {time.monotonic() - t0:.0f} s", flush=True)
    print(f"answered {n_ok}/{len(todo)} in {time.monotonic() - t0:.0f} s", flush=True)
    return 0 if n_ok == len(todo) else 1


if __name__ == "__main__":
    sys.exit(main())
