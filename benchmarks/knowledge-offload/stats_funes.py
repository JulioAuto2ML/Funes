#!/usr/bin/env python3
"""The `funes` rows (D1): the deployed agent's loop against the plain `store` condition, per size, IQ4_XS.

  python3 stats_funes.py [--results DIR] [--out results/FUNES_TABLE.md]

One cell per size (`...-funes-pool14267-r0.json`), 1,000 unambiguous items, the same questions as the `store` cells.
Before any statistic each cell is checked: 1,000 distinct ids equal to the store cell's, the served model sha256-verified
and the planned base, no adapter, `funes_pool` 14,267; a failed check is an exit 1 with nothing written.
Differences are paired (funes - store on `contains`); 95% percentile bootstrap over questions, 10,000 resamples,
seed 0, one resample shared by every size (as stats_e1). Not in the Holm family: the rows are descriptive (D1, one repeat).
Reported beside accuracy: errors (counted wrong), the share of answers that called any tool (the summary's `lookup_rate`, which for the agent counts every
tool, not only its memory lookup) with the number of answers that called `recall` and `web_search`, taint rate, wall time, context overflow count.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
SIZES = ["0.8b", "2b", "4b", "9b", "27b"]
NB, SEED = 10000, 0


def cell(results: Path, size: str, cond: str) -> dict:
    tail = "funes-pool14267" if cond == "funes" else cond
    return json.loads((results / f"popqa-stratified1u-n1000-qwen3.5-{size}-iq4_xs-{tail}-r0.json").read_text())


def check(c: dict, size: str, cond: str) -> list[str]:
    bad = []
    ids = [a["id"] for a in c["answers"]]
    if len(ids) != 1000 or len(set(ids)) != 1000:
        bad.append(f"{size} {cond}: {len(ids)} answers, {len(set(ids))} distinct ids")
    if c["model"].get("verified") != "sha256" or c["model"]["id"] != f"qwen3.5-{size}-iq4_xs":
        bad.append(f"{size} {cond}: model {c['model'].get('id')} verified={c['model'].get('verified')}")
    if c.get("adapters"):
        bad.append(f"{size} {cond}: an adapter is applied")
    if cond == "funes" and c.get("funes_pool") != 14267:
        bad.append(f"{size} funes: funes_pool {c.get('funes_pool')}")
    return bad


def report(results: Path) -> str:
    cells, bad = {}, []
    for s in SIZES:
        for cond in ("funes", "store"):
            c = cell(results, s, cond)
            bad += check(c, s, cond)
            cells[(s, cond)] = c
        if {a["id"] for a in cells[(s, "funes")]["answers"]} != {a["id"] for a in cells[(s, "store")]["answers"]}:
            bad.append(f"{s}: funes and store cover different questions")
    if bad:
        raise SystemExit("verification failed:\n  " + "\n  ".join(bad))
    ids = sorted(a["id"] for a in cells[("0.8b", "funes")]["answers"])
    idx = np.random.default_rng(SEED).integers(0, len(ids), size=(NB, len(ids)))

    def vec(c):
        m = {a["id"]: int(a["contains"]) for a in c["answers"]}
        return np.array([m[i] for i in ids], dtype=float)

    L = ["# `funes` rows: the deployed agent against the plain store", "",
         "1,000 unambiguous PopQA items, IQ4_XS, one repeat each, `contains`. Differences are paired, 95% bootstrap over questions "
         f"({NB} resamples, seed {SEED}, one resample shared by all sizes). Descriptive: outside the Holm family. "
         "Errors are counted wrong (not re-run).", "",
         "| size | store | funes | funes - store [95% CI] | only store right | only funes right | errors | any-tool rate | `recall` | `web_search` | taint rate | wall (s) |",
         "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for s in SIZES:
        f, st = cells[(s, "funes")], cells[(s, "store")]
        a, b = vec(f), vec(st)
        d = a[idx].mean(axis=1) - b[idx].mean(axis=1)
        lo, hi = np.percentile(d, [2.5, 97.5])
        only_s, only_f = int(((b == 1) & (a == 0)).sum()), int(((a == 1) & (b == 0)).sum())
        sm = f["summary"]["pass1"]
        used = lambda t: sum(1 for x in f["answers"] if t in x["tools"])
        L.append(f"| {s} | {b.mean():.3f} | {a.mean():.3f} | {a.mean() - b.mean():+.3f} [{lo:+.3f}, {hi:+.3f}] | {only_s} | {only_f} | "
                 f"{sum(1 for x in f['answers'] if x.get('error'))} | {sm['lookup_rate']:.3f} | {used('recall')} | {used('web_search')} | {sm['tainted']:.3f} | {f['wall_s']:.0f} |")
    L += ["", "By popularity bucket (funes accuracy, n in brackets; buckets 1e0, 1e6, 1e7 are too small to read):", "",
          "| size | " + " | ".join(f"1e{k}" for k in range(1, 6)) + " |", "|---|" + "---|" * 5]
    for s in SIZES:
        row = []
        for k in range(1, 6):
            xs = [a for a in cells[(s, 'funes')]["answers"] if a["pop_bucket"] == f"1e{k}"]
            row.append(f"{np.mean([a['contains'] for a in xs]):.3f} [{len(xs)}]")
        L.append(f"| {s} | " + " | ".join(row) + " |")
    over = {s: sum(1 for x in cells[(s, "funes")]["answers"] if "exceeds the available context" in str(x.get("error"))) for s in SIZES}
    L += ["", "Context-overflow errors (`request ... exceeds the available context`), counted wrong: "
          + ", ".join(f"{s} {n}" for s, n in over.items()) + f" (total {sum(over.values())} of 5000).", ""]
    return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=str(HERE / "results"))
    ap.add_argument("--out", default=str(HERE / "results" / "FUNES_TABLE.md"))
    a = ap.parse_args(argv)
    text = report(Path(a.results))
    Path(a.out).write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
