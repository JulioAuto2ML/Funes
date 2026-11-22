#!/usr/bin/env python3
"""E5 statistics (E5_DESIGN.md section 4): the Holm family H9a, H9b, H9c (m = 3), and the reported-only tables.

  python3 verify_e5.py && python3 stats_e5.py [--results DIR] [--out results/E5_TABLE.md]

Raw p-values, 10,000 question bootstrap resamples, seed 0 (harness seed), one-sided as in stats_holm.py (p = (1 + #) / (1 + B)):
  H9a  L1 2B `tool` default-prompt lookup rate >= 0.80 on the full 1,000 and on the held-out relations; p = max over the two of
       the share of resamples with rate <= 0.80. Holds when both point estimates >= 0.80 and adjusted p < 0.05.
  H9b  acc(L1, `tool`) - acc(L0, `tool`) paired over the same 1,000 ids; p = share of resamples with diff <= 0. Holds when the
       95% CI lower bound > 0, the point difference >= +0.10 and adjusted p < 0.05.
  H9c  GSM8K `closed`, L1 - base (committed base cell), paired over 500 ids; equivalence at +-0.05 as TOST (stats_holm.py's H5a):
       p = max of the two one-sided shares. Holds when the 95% CI lies inside +-0.05 and adjusted p < 0.05.
The rule for "holds" is the design's text plus the Holm gate; the p-value definitions above were not spelled out in the design
(written before the data, here). H4 is untested and is not in this family. Everything else printed is reported only.
No result is read before verify_e5.py passes (the caller's job). Nothing is tuned.
"""
import argparse
import json
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ko import popqa  # noqa: E402
from stats_holm import holm  # noqa: E402

NB, SEED, BAR, MARGIN = 10_000, 0, 0.80, 0.05
HELDOUT = ("director", "father", "screenwriter")
R = HERE / "results"


def popqa_cell(results: Path, tag: str, cond="tool", variant="", extra="", n=1000):
    name = f"popqa-stratified1u{extra}-n{n}-{tag}-{cond}{variant}-r0.json"
    return json.loads((results / name).read_text())


def rows(cell):
    return {x["id"]: x for x in cell["answers"] if x.get("pass", 1) == 1}


def idx(n):
    return np.random.RandomState(SEED).randint(0, n, size=(NB, n))


def boot_means(v):
    v = np.asarray(v, dtype=float)
    return v[idx(len(v))].mean(axis=1)


def share(b, pred):
    return float((np.sum(pred(b)) + 1) / (NB + 1))


def ci(b):
    return float(np.percentile(b, 2.5)), float(np.percentile(b, 97.5))


def paired(a: dict, b: dict):
    ids = sorted(set(a) & set(b))
    d = np.array([float(a[i]) - float(b[i]) for i in ids])
    return ids, d


def h9(results: Path, props: dict):
    out = {}
    l1, l0 = rows(popqa_cell(results, "qwen3.5-2b-iq4_xs-l1")), rows(popqa_cell(results, "qwen3.5-2b-iq4_xs-l0"))
    full = [bool(x["tools"]) for x in l1.values()]
    held = [bool(x["tools"]) for i, x in l1.items() if props[i] in HELDOUT]
    parts = {}
    for name, v in (("full", full), ("held-out", held)):
        b = boot_means(v)
        parts[name] = {"n": len(v), "rate": float(np.mean(v)), "ci": ci(b), "p": share(b, lambda b: b <= BAR)}
    out["H9a"] = {"parts": parts, "p": max(p["p"] for p in parts.values()),
                  "met": all(p["rate"] >= BAR for p in parts.values())}
    ids, d = paired({i: l1[i]["contains"] for i in l1}, {i: l0[i]["contains"] for i in l0})
    b = boot_means(d)
    out["H9b"] = {"n": len(ids), "diff": float(d.mean()), "ci": ci(b), "p": share(b, lambda b: b <= 0),
                  "met": ci(b)[0] > 0 and d.mean() >= 0.10}
    g = lambda tag: {x["id"]: x["correct"] for x in json.loads((results / f"gsm8k-n500-s1-{tag}-closed-r0.json").read_text())["answers"]}
    ids, d = paired(g("qwen3.5-2b-iq4_xs-l1"), g("qwen3.5-2b-iq4_xs"))
    b = boot_means(d)
    out["H9c"] = {"n": len(ids), "diff": float(d.mean()), "ci": ci(b),
                  "p": max(share(b, lambda b: b <= -MARGIN), share(b, lambda b: b >= MARGIN)),
                  "met": ci(b)[0] > -MARGIN and ci(b)[1] < MARGIN, "discordant": float((d != 0).mean())}
    adj = holm({k: out[k]["p"] for k in out})
    for k in out:
        out[k]["adj_p"] = adj[k]
        out[k]["holds"] = bool(out[k]["met"] and adj[k] < 0.05)
    return out


def per_group(cells: dict, props: dict, key):
    """{group: {cell label: (n, lookup rate, acc)}}"""
    t = defaultdict(dict)
    for lab, rs in cells.items():
        g = defaultdict(list)
        for i, x in rs.items():
            g[key(i, x)].append(x)
        for k, xs in g.items():
            t[k][lab] = (len(xs), sum(bool(x["tools"]) for x in xs) / len(xs), sum(x["contains"] for x in xs) / len(xs))
    return t


def fmt_row(label, vals):
    return "| %s | %s |" % (label, " | ".join("%d: %.3f / %.3f" % v if v else "-" for v in vals))


def base_noise(results: Path) -> str:
    """Base-vs-base on GSM8K `closed` (2B IQ4_XS): the committed cell against a rerun (tag `...-rerun`), paired over the 500 ids."""
    a = results / "gsm8k-n500-s1-qwen3.5-2b-iq4_xs-closed-r0.json"
    b = results / "gsm8k-n500-s1-qwen3.5-2b-iq4_xs-rerun-closed-r0.json"
    if not b.exists():
        return "The base-vs-base rate (same model, same prompt, run twice) was NOT measured: no rerun cell is committed."
    ra = {x["id"]: x for x in json.loads(a.read_text())["answers"]}
    rb = {x["id"]: x for x in json.loads(b.read_text())["answers"]}
    if set(ra) != set(rb):
        raise SystemExit("rerun and committed base cell cover different problems")
    disc = sum(bool(ra[i]["correct"]) != bool(rb[i]["correct"]) for i in ra) / len(ra)
    same = sum(ra[i]["prediction"] == rb[i]["prediction"] for i in ra)
    return (f"Base-vs-base (same model, prompt and settings, run twice, `{b.name}`): discordant {disc:.3f} of {len(ra)}; "
            f"{same} of {len(ra)} generated texts identical. Greedy decoding on this server is reproducible here, so the run-to-run noise is "
            f"{'zero' if same == len(ra) else 'not zero'} in this one repeat and the L1-vs-base disagreement is the adapter's, not noise "
            "(one repeat, one server build; not a claim about other builds or batch sizes).")


def report(results: Path) -> str:
    items = popqa.load(popqa.DEFAULT_PATH)
    props = {i.id: i.prop for i in items}
    h = h9(results, props)
    L = ["# E5 results (stats_e5.py)", "", f"{NB} bootstrap resamples, seed {SEED}. Holm family m = 3 (H9a, H9b, H9c); H4 untested, not in it.", "",
         "## Holm family", "", "| test | estimate | 95% CI | raw p | Holm adj. p | holds |", "|---|---|---|---|---|---|"]
    for name, p in h["H9a"]["parts"].items():
        L.append(f"| H9a {name} (n={p['n']}) lookup rate | {p['rate']:.3f} | [{p['ci'][0]:.3f}, {p['ci'][1]:.3f}] | {p['p']:.2g} | | |")
    for k, what in (("H9a", "H9a (both parts)"), ("H9b", "H9b acc(L1)-acc(L0), tool"), ("H9c", "H9c GSM8K L1-base, closed")):
        e = h[k]
        est = "-" if k == "H9a" else f"{e['diff']:+.3f}"
        c = "-" if k == "H9a" else f"[{e['ci'][0]:+.3f}, {e['ci'][1]:+.3f}]"
        L.append(f"| {what} | {est} | {c} | {e['p']:.2g} | {e['adj_p']:.2g} | {'HOLDS' if e['holds'] else 'does not hold'} |")
    d = h["H9c"]["discordant"]
    L += ["", f"H9c noise: L1 and base disagree on {d:.3f} of the 500 problems. A 95% CI half-width <= {MARGIN} at n = 500 needs a discordant "
          f"rate <= about {MARGIN ** 2 * 500 / 1.96 ** 2:.3f}. " + base_noise(results)]
    cells = {}
    for size in ("2b", "0.8b"):
        b = f"qwen3.5-{size}-iq4_xs"
        for lab, tag, kw in ((f"{size} base", b, {}), (f"{size} L1", b + "-l1", {}), (f"{size} L0", b + "-l0", {})):
            try:
                cells[lab] = rows(popqa_cell(results, tag, **kw))
            except FileNotFoundError:
                pass
    labs = list(cells)
    L += ["## `tool`, default prompt: lookup rate / accuracy by relation (n: lookup / acc)", "", "| relation | " + " | ".join(labs) + " |", "|---|" + "---|" * len(labs)]
    for rel, vals in sorted(per_group(cells, props, lambda i, x: props[i] + (" *held-out*" if props[i] in HELDOUT else "")).items()):
        L.append(fmt_row(rel, [vals.get(l) for l in labs]))
    L += ["", "## by popularity bucket", "", "| bucket | " + " | ".join(labs) + " |", "|---|" + "---|" * len(labs)]
    for bk, vals in sorted(per_group(cells, props, lambda i, x: x["pop_bucket"]).items()):
        L.append(fmt_row(bk, [vals.get(l) for l in labs]))
    L += ["", "## Held-out relations only (director, father, screenwriter)", "", "| | " + " | ".join(labs) + " |", "|---|" + "---|" * len(labs)]
    ho = per_group({l: {i: x for i, x in r.items() if props[i] in HELDOUT} for l, r in cells.items()}, props, lambda i, x: "held-out")
    L.append(fmt_row("all three", [ho["held-out"].get(l) for l in labs]))
    L += ["", "## Other cells (reported only; n: lookup rate / accuracy)", "", "| cell | 2B | 0.8B |", "|---|---|---|"]

    def one(tag, **kw):
        try:
            c = popqa_cell(results, tag, **kw)
        except FileNotFoundError:
            return "-"
        s = c["summary"]["pass1"]
        o = s.get("outcomes")
        return "%d: %.3f / %.3f%s" % (s["n"], s["lookup_rate"], s["acc"], "" if not o else " (correct %.3f, abstained %.3f, hallucinated %.3f)" % (o["correct"], o["abstained"], o["hallucinated"]))
    for lab, arm, kw in (("L1 closed", "l1", dict(cond="closed")), ("L1 tool_free", "l1", dict(variant="-tool_free")),
                         ("L1 tool_describe", "l1", dict(variant="-tool_describe")),
                         ("L1 abstain_default (ablated, n=200)", "l1", dict(variant="-abstain_default", extra="-ablated", n=200)),
                         ("L1 set P (n=200)", "l1", dict(extra="-P", n=200)), ("L0 set P (n=200)", "l0", dict(extra="-P", n=200))):
        L.append(f"| {lab} | {one('qwen3.5-2b-iq4_xs-' + arm, **kw)} | {one('qwen3.5-0.8b-iq4_xs-' + arm, **kw)} |")
    L.append(f"| L1 on 2B Q8_0 (transfer) | {one('qwen3.5-2b-q8_0-l1')} | - |")
    for lab, tag in (("base closed", "qwen3.5-%s-iq4_xs"),):
        L.append(f"| {lab} | {one(tag % '2b', cond='closed')} | {one(tag % '0.8b', cond='closed')} |")
    L += ["", "## GSM8K `closed`, n = 500", "", "| | 2B | 0.8B |", "|---|---|---|"]
    for lab, suf in (("base", ""), ("L1", "-l1"), ("L0", "-l0")):
        acc = []
        for size in ("2b", "0.8b"):
            c = json.loads((results / f"gsm8k-n500-s1-qwen3.5-{size}-iq4_xs{suf}-closed-r0.json").read_text())
            acc.append("%.3f" % c["summary"]["acc"])
        L.append(f"| {lab} | {acc[0]} | {acc[1]} |")
    return "\n".join(L) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default=str(R))
    ap.add_argument("--out", default=str(R / "E5_TABLE.md"))
    a = ap.parse_args(argv)
    text = report(Path(a.results))
    Path(a.out).write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
