#!/usr/bin/env python3
"""Do two repeats of one cell give the same answers? Run on 100 items before any repeat is trusted.

  identity_check.py results/<cell>-r0.json results/<cell>-r1.json

Greedy decoding with one slot should be bit-identical (E0's five harness conditions were: 0 of
1000 predictions differed). A size or build where it is not needs its repeats counted, not assumed.
Exit 0 when every prediction matches, 1 otherwise.
"""
import json
import sys


def compare(a: dict, b: dict) -> dict:
    ra = {(x["id"], x["pass"]): x["prediction"] for x in a["answers"]}
    rb = {(x["id"], x["pass"]): x["prediction"] for x in b["answers"]}
    common = ra.keys() & rb.keys()
    diff = sorted(k for k in common if ra[k] != rb[k])
    return {"compared": len(common), "only_in_one": len(ra.keys() ^ rb.keys()), "differ": len(diff),
            "examples": [(k, ra[k], rb[k]) for k in diff[:5]]}


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    r = compare(*(json.load(open(p)) for p in argv[1:]))
    print(f"{r['compared']} compared, {r['differ']} differ, {r['only_in_one']} in only one file")
    for k, x, y in r["examples"]:
        print(f"  {k}: {x!r} vs {y!r}")
    return 0 if r["differ"] == 0 and r["only_in_one"] == 0 and r["compared"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
