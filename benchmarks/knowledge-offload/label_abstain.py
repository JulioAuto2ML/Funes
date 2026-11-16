#!/usr/bin/env python3
"""Check the abstain regex of the `ablated` arm against hand labels, before any number is reported.

  dump   pick 50 non-correct answers from ablated result files (half the regex calls
         `abstained`, half `hallucinated`, so a miss shows in both directions) into a TSV
         with a blank `human` column;
  score  read the filled TSV: agreement, confusion matrix, every disagreement.

Fill `human` with abstained or hallucinated (or correct if the alias is really there).
No model is involved.
"""
import argparse
import csv
import json
import random
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ko import popqa                                                    # noqa: E402

FIELDS = ["file", "id", "condition", "question", "gold", "prediction", "regex", "human"]
LABELS = ("abstained", "hallucinated", "correct")


def pick(rows: list[dict], n: int, seed: int) -> list[dict]:
    rng = random.Random(seed)
    by = {k: [r for r in rows if r["regex"] == k] for k in ("abstained", "hallucinated")}
    half = n // 2
    take_a = min(half, len(by["abstained"]))
    take_h = min(n - take_a, len(by["hallucinated"]))
    take_a = min(n - take_h, len(by["abstained"]))
    out = rng.sample(by["abstained"], take_a) + rng.sample(by["hallucinated"], take_h)
    rng.shuffle(out)
    return out


def collect(files: list[Path], items_by_id: dict) -> list[dict]:
    rows = []
    for f in files:
        r = json.loads(f.read_text())
        for a in r["answers"]:
            if a.get("outcome") in ("abstained", "hallucinated"):
                it = items_by_id.get(a["id"])
                rows.append({"file": f.name, "id": a["id"], "condition": r["condition"],
                             "question": it.question if it else "", "gold": " | ".join(it.aliases[:3]) if it else "",
                             "prediction": a["prediction"].replace("\t", " ").replace("\n", " "),
                             "regex": a["outcome"], "human": ""})
    return rows


def agreement(rows: list[dict]) -> dict:
    done = [r for r in rows if r["human"].strip()]
    bad = [r for r in done if r["human"].strip() not in LABELS]
    if bad:
        raise ValueError(f"unknown label {bad[0]['human']!r} (use {', '.join(LABELS)})")
    conf = Counter((r["regex"], r["human"].strip()) for r in done)
    agree = sum(v for (a, b), v in conf.items() if a == b)
    return {"labelled": len(done), "of": len(rows), "agree": agree,
            "rate": agree / len(done) if done else None, "confusion": dict(conf),
            "disagreements": [r for r in done if r["regex"] != r["human"].strip()]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump")
    d.add_argument("--glob", default="popqa-*ablated*.json")
    d.add_argument("--n", type=int, default=50)
    d.add_argument("--seed", type=int, default=0)
    d.add_argument("--out", default=str(HERE / "results" / "abstain_labels.tsv"))
    d.add_argument("--data", default=str(popqa.DEFAULT_PATH))
    s = sub.add_parser("score")
    s.add_argument("tsv")
    s.add_argument("--min", type=float, default=0.9, help="agreement below this exits 1")
    a = ap.parse_args()
    if a.cmd == "dump":
        files = sorted((HERE / "results").glob(a.glob))
        if not files:
            sys.exit(f"no files match results/{a.glob}")
        by_id = {i.id: i for i in popqa.load(Path(a.data))}
        chosen = pick(collect(files, by_id), a.n, a.seed)
        with open(a.out, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, FIELDS, delimiter="\t")
            w.writeheader()
            w.writerows(chosen)
        print(f"{len(chosen)} answers -> {a.out}; fill the `human` column, then: label_abstain.py score {a.out}")
        return
    with open(a.tsv, encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    r = agreement(rows)
    print(f"labelled {r['labelled']} of {r['of']}; regex agrees on {r['agree']} ({r['rate']:.1%})"
          if r["rate"] is not None else "nothing labelled yet")
    for (rx, hu), v in sorted(r["confusion"].items()):
        print(f"  regex {rx:<13} human {hu:<13} {v}")
    for x in r["disagreements"]:
        print(f"  DISAGREE {x['condition']} #{x['id']}: {x['prediction']!r}  regex={x['regex']} human={x['human']}")
    if r["rate"] is None or r["rate"] < a.min or r["labelled"] < 50:
        sys.exit(1)


if __name__ == "__main__":
    main()
