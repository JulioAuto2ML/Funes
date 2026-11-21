#!/usr/bin/env python3
"""E5 dev measurement: the harness's `tool` cell (SYSTEM_TOOL, the lookup tool, the same loop and scoring) on the dev items.

  python3 e5_dev_eval.py --dev data/e5/build_anchor_2b/dev_items.jsonl --llm-url http://localhost:8199 --allow-local --out dev_A.json

Nothing new is measured here: dev items become `popqa.Item`s and go through `runner.run_cell` unchanged, against the whole
PopQA store (same distractors as the eval cells). The output keeps every answer plus lookup rate and accuracy overall and per
popularity bucket; it never touches the eval items. Resumable per run (writes once at the end; a dev run is 300 items).
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ko import conditions as C, popqa, runner  # noqa: E402
from ko import tools as T  # noqa: E402
from ko.llm import Client  # noqa: E402


def dev_items(path: Path) -> list[popqa.Item]:
    """Dev rows -> Items. `obj` is the first alias; `s_pop` is rebuilt from the bucket (`1e3` -> 1000) so `pop_bucket` returns it."""
    out = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        r = json.loads(line)
        out.append(popqa.Item(id=r["id"], question=r["question"], subj=r["subj"], prop=r["prop"], obj=r["aliases"][0],
                              aliases=tuple(r["aliases"]), s_pop=int(10 ** int(r["bucket"][2:]))))
    return out


def by_bucket(rows: list[dict]) -> dict:
    out = {}
    for b in sorted({r["pop_bucket"] for r in rows}):
        rs = [r for r in rows if r["pop_bucket"] == b]
        out[b] = {"n": len(rs), "lookup_rate": sum(bool(r["tools"]) for r in rs) / len(rs), "acc": sum(r["contains"] for r in rs) / len(rs)}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", required=True)
    ap.add_argument("--llm-url", required=True)
    ap.add_argument("--allow-local", action="store_true")
    ap.add_argument("--data", default=str(popqa.DEFAULT_PATH))
    ap.add_argument("--variant", default="default", choices=C.VARIANTS)
    ap.add_argument("--n", type=int, default=0, help="first N dev items only (0 = all)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    items = dev_items(Path(a.dev))
    if a.n:
        items = items[:a.n]
    store = runner.build_store(popqa.load(Path(a.data)))
    client = Client(a.llm_url, allow_local=a.allow_local)
    ctx = C.Context(client=client, store=store, seed=0, notes=T.Notes(), variant=a.variant)
    res = runner.run_cell(condition="tool", items=items, ctx=ctx, rep=0, meta={"task": "e5-dev", "dev_file": str(a.dev),
                          "server": client.server_info(), "prompt_variant": a.variant})
    s = res["summary"]["pass1"]
    res["by_bucket"] = by_bucket(res["answers"])
    Path(a.out).write_text(json.dumps(res, indent=1, ensure_ascii=False))
    print(json.dumps({"n": s["n"], "lookup_rate": s["lookup_rate"], "acc": s["acc"], "errors": s["errors"], "wall_s": res["wall_s"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
