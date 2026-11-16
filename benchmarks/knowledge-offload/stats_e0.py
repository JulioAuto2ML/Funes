#!/usr/bin/env python3
"""Results table for one cell set: accuracy with CIs, paired comparisons, popularity buckets.

  python3 stats_e0.py                                   # E0 (9B distill), unambiguous items -> E0_TABLE_unambiguous.md
  python3 stats_e0.py --all-items --out results/E0_TABLE.md      # the full 1000, as first reported
  python3 stats_e0.py --prefix popqa-stratified1u --tag qwen3.5-9b-iq4_xs   # an E1 size

No model, no network. Cells are found by what is *inside* the result files (`model_tag`,
`condition`, `prompt_variant`, `funes_pool`, `arm`), not by file name, and grouped by label; repeats
are averaged per question (the greedy harness conditions are bit-identical across repeats; funes is
not). Primary metric: `contains`, on unambiguous items (subject+property with one object over all
14,267 triples); `--all-items` also counts homonym subjects, where a list of candidates is credited.
The `ablated` arm (own triple withheld) is reported in its own section, never mixed into the main one.
"""
import argparse
import json
import math
import random
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run as runner                                                    # noqa: E402
from ko import popqa                                                    # noqa: E402

TWO_PASS = ("web+cache", "web+writeback")
ORDER = ["closed", "oracle", "store", "tool", "tool+contract", "tool/search_first", "tool+contract/search_first",
         "funes", "web", "web/search_first", "web+cache", "web+cache/search_first", "web+writeback/search_first"]
PAIRS = [
    ("store", "closed"), ("oracle", "closed"), ("tool+contract", "closed"), ("tool/search_first", "closed"),
    ("tool", "closed"), ("funes", "closed"), ("web/search_first", "closed"),
    ("tool+contract", "store"), ("tool/search_first", "store"), ("tool/search_first", "tool+contract"),
    ("tool+contract", "tool"), ("funes", "store"), ("web/search_first", "store"),
    ("web/search_first", "tool/search_first"), ("web+cache/search_first p1", "web/search_first"),
    ("web+cache/search_first p2", "web+cache/search_first p1"), ("web+cache/search_first p2", "web/search_first"),
    ("web+cache/search_first p2", "store"),
    ("web+writeback/search_first p2", "web+writeback/search_first p1"),
    ("web+writeback/search_first p2", "web/search_first"), ("web+writeback/search_first p2", "store"),
]


def label(r: dict) -> str:
    lab = r["condition"]
    if r.get("prompt_variant", "default") != "default":
        lab += "/" + r["prompt_variant"]
    if r["condition"] == "funes" and r.get("funes_pool"):
        lab += f"@{r['funes_pool']}"
    return lab


def find_cells(results: Path, prefix: str, tag: str) -> dict[str, list[dict]]:
    """label -> result dicts (one per repeat). Selected by the files' own meta."""
    cells: dict[str, list[dict]] = {}
    for f in sorted(results.glob(f"{prefix}-n*-r*.json")) + sorted(results.glob(f"{prefix}-n*.json")):
        r = json.loads(f.read_text())
        if r.get("model_tag") != tag or r.get("arm", "main") != "main" or "answers" not in r:
            continue
        reps = cells.setdefault(label(r), [])
        if all(x["repeat"] != r["repeat"] for x in reps):
            reps.append(r)
    return cells


def find_ablated(results: Path, prefix: str, tag: str) -> dict[str, list[dict]]:
    cells: dict[str, list[dict]] = {}
    for f in sorted(results.glob(f"{prefix}-ablated-n*.json")):
        r = json.loads(f.read_text())
        if r.get("model_tag") == tag and r.get("arm") == "ablated" and "answers" in r:
            cells.setdefault(label(r), []).append(r)
    return cells


def passes(lab: str) -> list[int]:
    return [1, 2] if lab.split("/")[0] in TWO_PASS else [1]


def ci(xs, seed=0, n=2000):
    rnd = random.Random(seed)
    m = len(xs)
    bs = sorted(sum(xs[rnd.randrange(m)] for _ in range(m)) / m for _ in range(n))
    return bs[int(.025 * n)], bs[int(.975 * n) - 1]


def mcnemar(b, c):
    n = b + c
    if n == 0:
        return 1.0
    return min(1.0, 2 * sum(math.comb(n, k) for k in range(min(b, c) + 1)) / 2 ** n)


def sort_key(lab):
    base = lab.split(" ")[0].split("@")[0]
    return (ORDER.index(base) if base in ORDER else len(ORDER), lab)


def build(cells, keep_ids, with_hedge, ablated=None) -> list[str]:
    """Markdown lines. `cells` label -> reps; `keep_ids` None or the set of question ids to use."""
    series = {}          # series label -> {id: mean contains}, {id: mean contains-and-not-hedge}, {id: bucket}
    nreps = {}
    for lab, reps in cells.items():
        for p in passes(lab):
            name = lab if p == 1 and len(passes(lab)) == 1 else f"{lab} p{p}"
            per_id, adj, bucket = {}, {}, {}
            for r in reps:
                with_hedge(r)
                for a in r["answers"]:
                    if a["pass"] != p or (keep_ids is not None and a["id"] not in keep_ids):
                        continue
                    per_id.setdefault(a["id"], []).append(int(a["contains"]))
                    adj.setdefault(a["id"], []).append(int(a["contains"] and not a.get("hedge", False)))
                    bucket[a["id"]] = a["pop_bucket"]
            series[name] = ({i: sum(v) / len(v) for i, v in per_id.items()},
                            {i: sum(v) / len(v) for i, v in adj.items()}, bucket)
            nreps[name] = len(reps)
    names = sorted(series, key=sort_key)
    ids = sorted(set.intersection(*(set(series[n][0]) for n in names))) if names else []
    out = [f"Questions: {len(ids)} (the same in every row).", "", "## Accuracy", "",
           "| condition | reps | acc | 95% CI | acc, hedge-adj | lost to hedging |", "|---|---|---|---|---|---|"]
    for n in names:
        x = [series[n][0][i] for i in ids]
        h = [series[n][1][i] for i in ids]
        lo, hi = ci(x)
        out.append("| %s | %d | %.3f | [%.3f, %.3f] | %.3f | %.3f |" % (
            n, nreps[n], sum(x) / len(x), lo, hi, sum(h) / len(h), (sum(x) - sum(h)) / len(x)))
    out += ["", "## Paired differences (A − B, same questions)", "",
            "| A | B | Δ acc | 95% CI | A-only | B-only | McNemar p |", "|---|---|---|---|---|---|---|"]
    def resolve(n):                 # "funes" -> "funes@3000": the pool size is part of the label
        return n if n in series else next((m for m in names if m.startswith(n + "@")), None)
    pairs, seen = [], set()
    for a, b in list(PAIRS) + [(n, "closed") for n in names if n != "closed"]:
        a, b = resolve(a), resolve(b)
        if a and b and (a, b) not in seen:
            seen.add((a, b))
            pairs.append((a, b))
    for a, b in pairs:
        d = [series[a][0][i] - series[b][0][i] for i in ids]
        lo, hi = ci(d)
        ao = sum(1 for i in ids if round(series[a][0][i]) > round(series[b][0][i]))
        bo = sum(1 for i in ids if round(series[a][0][i]) < round(series[b][0][i]))
        out.append("| %s | %s | %+.3f | [%+.3f, %+.3f] | %d | %d | %.2g |" % (
            a, b, sum(d) / len(d), lo, hi, ao, bo, mcnemar(ao, bo)))
    bk = sorted({series[names[0]][2][i] for i in ids}, key=lambda s: int(s[2:])) if names else []
    out += ["", "## Accuracy by popularity bucket", "", "| condition | " + " | ".join(
        "%s (n=%d)" % (b, sum(1 for i in ids if series[names[0]][2][i] == b)) for b in bk) + " |",
        "|---|" + "---|" * len(bk)]
    for n in names:
        row = []
        for b in bk:
            v = [series[n][0][i] for i in ids if series[n][2][i] == b]
            row.append("%.3f" % (sum(v) / len(v)))
        out.append("| %s | %s |" % (n, " | ".join(row)))
    if ablated:
        out += ["", "## Ablated arm (own triple withheld, `abstain_ok` prompt)", "",
                "| condition | n | correct | abstained | hallucinated | 95% CI hallucinated |", "|---|---|---|---|---|---|"]
        for lab in sorted(ablated, key=sort_key):
            rows = [a for a in ablated[lab][0]["answers"] if a["pass"] == 1]
            hal = [int(a["outcome"] == "hallucinated") for a in rows]
            lo, hi = ci(hal)
            f = lambda o: sum(a["outcome"] == o for a in rows) / len(rows)
            out.append("| %s | %d | %.3f | %.3f | %.3f | [%.3f, %.3f] |" % (
                lab, len(rows), f("correct"), f("abstained"), f("hallucinated"), lo, hi))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", default="qwen3.8-9b-q8", help="the model_tag of the cells (default: E0's 9B distill)")
    ap.add_argument("--prefix", default="popqa", help="file-name prefix: popqa (E0), popqa-stratified1u, ...")
    ap.add_argument("--results", default=str(HERE / "results"))
    ap.add_argument("--data", default=str(popqa.DEFAULT_PATH))
    ap.add_argument("--all-items", action="store_true", help="do not restrict to unambiguous items")
    ap.add_argument("--out", default=None)
    a = ap.parse_args(argv)
    results = Path(a.results)
    all_items = popqa.load(Path(a.data))
    cand = popqa.candidate_sets(all_items)
    by_id = {i.id: i for i in all_items}
    keep = None if a.all_items else {i.id for i in popqa.unambiguous(all_items, cand)}
    cells = find_cells(results, a.prefix, a.tag)
    if not cells:
        sys.exit(f"no cells with model_tag={a.tag!r} under {results}/{a.prefix}-n*.json")
    first = next(iter(cells.values()))[0]
    meta = first.get("model")
    head = (f"{meta['id']} ({meta['repo']}, {meta['quant']}, lineage: {meta['lineage']}, sha256 {meta['sha256'][:12]}…)"
            if meta else f"{a.tag} (E0: no models.lock; the file is a community distill, inferred from its size)")
    out = [f"# Results — {head}", "",
           f"Cell set `{a.prefix}`, dataset seed {first.get('dataset_seed')}, sample {first.get('sample', 'random')}; "
           + ("all items, homonym subjects included (a list of candidates counts as correct)."
              if a.all_items else "**unambiguous items only** (primary metric)."),
           "Generated by `stats_e0.py`: `contains` scoring; CIs are percentile bootstrap over questions (2000); "
           "repeats averaged per question.", ""]
    out += build(cells, keep, lambda r: runner.with_hedge(r, cand, by_id), find_ablated(results, a.prefix, a.tag))
    dest = Path(a.out) if a.out else results / ("E0_TABLE.md" if a.all_items and a.prefix == "popqa" and a.tag == "qwen3.8-9b-q8"
                                                else f"{a.prefix}_{a.tag}_{'all' if a.all_items else 'unambiguous'}_TABLE.md")
    dest.write_text("\n".join(out) + "\n")
    print("\n".join(out))
    print(f"\n-> {dest}", file=sys.stderr)


if __name__ == "__main__":
    main()
