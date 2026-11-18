#!/usr/bin/env python3
"""Did every cell of the default-prompt variants queue (stages A and B) get written, whole and clean?

  python3 verify_variants.py [--results DIR]       # exit 0 only if all 12 cells check out

Expected: condition `tool`, variants tool_first / tool_free / tool_describe, for qwen3.5 9B and 4B IQ4_XS (stage A) and 4B and
9B Q8_0 (stage B), 1000 answers each, checked from the file's own meta: sample stratified, dataset seed 1, ambiguous items
excluded, model hash verified (`model.verified == "sha256"`), 0 answers with an `error`. Cells are matched by `model_tag`,
condition, variant and arm, not by name; the baseline `default` cells never count.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TAGS = ["qwen3.5-9b-iq4_xs", "qwen3.5-4b-iq4_xs", "qwen3.5-4b-q8_0", "qwen3.5-9b-q8_0"]
VARIANTS = ["tool_first", "tool_free", "tool_describe"]
N = 1000


def check(results: Path) -> tuple[list[tuple], int]:
    found = {}
    for f in sorted(results.glob("popqa-stratified1u-n*-r0.json")):
        r = json.loads(f.read_text())
        found[(r.get("model_tag"), r.get("condition"), r.get("prompt_variant", "default"), r.get("arm", "main"))] = (f, r)
    rows, bad = [], 0
    for tag in TAGS:
        for var in VARIANTS:
            hit = found.get((tag, "tool", var, "main"))
            if not hit:
                rows.append((tag, var, "tool", "MISSING", "")); bad += 1
                continue
            f, r = hit
            a = [x for x in r["answers"] if x["pass"] == 1]
            why = []
            if len(a) != N or r.get("n_questions") != N:
                why.append(f"{len(a)} answers (n_questions {r.get('n_questions')}), expected {N}")
            errs = sum(1 for x in a if x.get("error"))
            if errs:
                why.append(f"{errs} answers with an error")
            if (r.get("model") or {}).get("verified") != "sha256":
                why.append(f"model not sha256-verified ({(r.get('model') or {}).get('verified')})")
            if r.get("sample") != "stratified" or r.get("dataset_seed") != 1 or not r.get("exclude_ambiguous"):
                why.append("not the E1 sample (stratified, seed 1, unambiguous)")
            if why:
                bad += 1
            rows.append((tag, var, "tool", "BAD" if why else "ok", "; ".join(why) or f"{len(a)} answers, {f.name}"))
    return rows, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default=str(HERE / "results"))
    a = ap.parse_args()
    rows, bad = check(Path(a.results))
    for r in rows:
        print("%-22s %-14s %-6s %-8s %s" % r)
    ok = sum(1 for r in rows if r[3] == "ok")
    print(f"\n{ok} of {len(rows)} cells ok; {bad} missing or bad")
    sys.exit(0 if bad == 0 and ok == len(rows) == 12 else 1)


if __name__ == "__main__":
    main()
