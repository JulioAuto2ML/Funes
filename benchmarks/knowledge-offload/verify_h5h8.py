#!/usr/bin/env python3
"""Did every default-prompt cell of the D12/D13/D15(1) queue get written, whole and clean?

  python3 verify_h5h8.py [--results DIR]          # exit 0 only if all 36 cells check out

Expected: for each of 9 model files (Qwen3.5 0.8/2/4/9/27B IQ4_XS, 0.8/2/4/9B Q8_0) the cells `tool` and `tool+contract`
under prompt variant `default` (main arm, 1000 answers) and under `abstain_default` (ablated arm, 200 answers). Each is
checked from the file's own meta: sample stratified, dataset seed 1, ambiguous items excluded, model hash verified
(`model.verified == "sha256"`), the expected number of answers, and 0 answers with an `error`.
Cells are matched by `model_tag`, condition, variant and arm, not by name.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TAGS = [f"qwen3.5-{s}-iq4_xs" for s in ("0.8b", "2b", "4b", "9b", "27b")] + [f"qwen3.5-{s}-q8_0" for s in ("0.8b", "2b", "4b", "9b")]
CONDS = ("tool", "tool+contract")
PLAN = [(arm, var, n) for arm, var, n in (("main", "default", 1000), ("ablated", "abstain_default", 200))]


def check(results: Path) -> tuple[list[tuple], int]:
    found = {}
    for f in sorted(results.glob("popqa-stratified1u*-r0.json")):
        r = json.loads(f.read_text())
        found[(r.get("model_tag"), r.get("condition"), r.get("prompt_variant", "default"), r.get("arm", "main"))] = (f, r)
    rows, bad = [], 0
    for tag in TAGS:
        for arm, var, n in PLAN:
            for cond in CONDS:
                hit = found.get((tag, cond, var, arm))
                if not hit:
                    rows.append((tag, arm, cond, var, "MISSING", "")); bad += 1
                    continue
                f, r = hit
                a = [x for x in r["answers"] if x["pass"] == 1]
                why = []
                if len(a) != n or r.get("n_questions") != n:
                    why.append(f"{len(a)} answers (n_questions {r.get('n_questions')}), expected {n}")
                errs = sum(1 for x in a if x.get("error"))
                if errs:
                    why.append(f"{errs} answers with an error")
                if (r.get("model") or {}).get("verified") != "sha256":
                    why.append(f"model not sha256-verified ({(r.get('model') or {}).get('verified')})")
                if r.get("sample") != "stratified" or r.get("dataset_seed") != 1 or not r.get("exclude_ambiguous"):
                    why.append("not the E1 sample (stratified, seed 1, unambiguous)")
                s = r.get("search") or {}
                if s.get("errors"):
                    why.append(f"{s['errors']} search errors")
                if why:
                    bad += 1
                rows.append((tag, arm, cond, var, "BAD" if why else "ok", "; ".join(why) or f"{len(a)} answers, {f.name}"))
    return rows, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default=str(HERE / "results"))
    a = ap.parse_args()
    rows, bad = check(Path(a.results))
    for r in rows:
        print("%-22s %-8s %-14s %-16s %-8s %s" % r)
    ok = sum(1 for r in rows if r[4] == "ok")
    print(f"\n{ok} of {len(rows)} cells ok; {bad} missing or bad")
    sys.exit(0 if bad == 0 and ok == len(rows) == 36 else 1)


if __name__ == "__main__":
    main()
