#!/usr/bin/env python3
"""E5 gate 3(b): does a LoRA GGUF served by llama-server do what the design needs of it?

  python3 e5_lora_gate.py --base-url http://localhost:8080 --lora-url http://localhost:8199 --dev DEV.jsonl --allow-local --out gate.json

`--base-url` is the plain IQ4_XS server, `--lora-url` the same GGUF started with `--lora ADAPTER.gguf --lora-init-without-apply`
(so the adapter can be set to any scale over HTTP without a restart). For the first N dev questions (greedy, the `tool` cell's
system prompt and lookup schema, one model turn each) it checks:
  * scale 0: every reply (text and tool calls) equals the base server's: the adapter, loaded but switched off, changes nothing;
  * scale 1: at least one reply differs from base (the adapter acts), and the number of replies that call `lookup` is reported.
Pass = scale 0 identical on all N and scale 1 differs on at least one. Nothing is interpreted beyond that.
"""
import argparse
import json
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ko import conditions as C, tools as T  # noqa: E402
import e5_dev_eval as D  # noqa: E402


def _post(url: str, body, timeout: float = 300):
    req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def reply(url: str, question: str) -> dict:
    """The first model turn of the `tool` cell, greedy: what the model said and which call it made."""
    body = {"messages": [{"role": "system", "content": C.SYSTEM_TOOL}, {"role": "user", "content": question}],
            "tools": [T.LOOKUP], "temperature": 0, "seed": 0, "max_tokens": 128}
    m = _post(url + "/v1/chat/completions", body)["choices"][0]["message"]
    calls = [(c["function"]["name"], c["function"]["arguments"]) for c in (m.get("tool_calls") or [])]
    return {"content": m.get("content") or "", "calls": calls}


def set_scale(url: str, scale: float) -> list:
    cur = json.loads(urllib.request.urlopen(url + "/lora-adapters", timeout=30).read())
    _post(url + "/lora-adapters", [{"id": a["id"], "scale": scale} for a in cur])
    return json.loads(urllib.request.urlopen(url + "/lora-adapters", timeout=30).read())


def compare(base: list[dict], other: list[dict]) -> dict:
    diff = [i for i, (a, b) in enumerate(zip(base, other)) if a != b]
    return {"n": len(base), "identical": len(base) - len(diff), "differing": diff,
            "lookup_calls": sum(bool(o["calls"]) for o in other)}


def verdict(zero: dict, one: dict) -> bool:
    return zero["identical"] == zero["n"] and bool(one["differing"])


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", required=True)
    ap.add_argument("--lora-url", required=True)
    ap.add_argument("--dev", required=True)
    ap.add_argument("--n", type=int, default=20)
    ap.add_argument("--allow-local", action="store_true", help="accepted for symmetry with the other scripts; this one talks to the URLs given")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    qs = [it.question for it in D.dev_items(Path(a.dev))[:a.n]]
    base = [reply(a.base_url, q) for q in qs]
    s0 = set_scale(a.lora_url, 0.0)
    zero = compare(base, [reply(a.lora_url, q) for q in qs])
    s1 = set_scale(a.lora_url, 1.0)
    one_replies = [reply(a.lora_url, q) for q in qs]
    one = compare(base, one_replies)
    ok = verdict(zero, one)
    rep = {"scale0": zero, "scale1": one, "adapters_scale0": s0, "adapters_scale1": s1, "pass": ok,
           "base_first": base[:3], "lora_first": one_replies[:3]}
    Path(a.out).write_text(json.dumps(rep, indent=1, ensure_ascii=False))
    print(json.dumps({"pass": ok, "scale0_identical": f"{zero['identical']}/{zero['n']}", "scale1_differing": len(one["differing"]),
                      "scale1_lookup_calls": one["lookup_calls"]}))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
