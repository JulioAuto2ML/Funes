#!/usr/bin/env python3
"""H5 (a, b) and H8 on the 36 default-prompt cells (D12, D13, D14, D16 in NOTEBOOK.md).

  python3 stats_h5h8.py                 # -> results/H5H8_TABLE.md

No model, no network. Cells (read by what is inside them, via stats_e0.find_cells / find_ablated):
  main    `tool` and `tool+contract` with the default prompt (labels without a `/prompt` suffix), 1000 questions;
  ablated `tool/abstain_default` and `tool+contract/abstain_default`, first 200 questions.
Tests (all on the same questions in both arms, so paired; 10k bootstrap resamples, one resample shared by every
size so slopes and pooled differences are paired too):
  H5a   TOST equivalence of `tool+contract` - `tool` at +-2 pts: p = max of the two one-sided bootstrap shares.
        Eligible sizes (D12): contract lookup rate >= .99 and tool lookup rate >= .8. Pooled = mean of the eligible
        sizes' per-question differences (equal n), per quant. Every size is shown, eligible or not.
  H5b   hallucination `tool+contract` vs `tool` on `ablated`: exact two-sided sign test on discordant questions per size;
        pooled over sizes = Mantel-Haenszel for matched pairs (sum of discordants; exact and chi-square both shown).
        The same 200 questions occur at every size, so the pooled p assumes more independence than there is.
  H8    OLS slope of lookup rate on log10(params), question-bootstrap CI, per quant; no p-value, outside Holm.
"""
import hashlib
import sys
from math import erfc, sqrt
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import stats_e0 as S0                                                   # noqa: E402
import stats_e1 as E1                                                   # noqa: E402
from stats_holm import sign_p                                           # noqa: E402

MARGIN = 0.02
LOOKUP_TC_MIN, LOOKUP_TOOL_MIN = 0.99, 0.8
ABL_TOOL, ABL_TC = "tool/abstain_default", "tool+contract/abstain_default"


def tost_p(d, idx, margin=MARGIN):
    """TOST on a paired per-question difference vector d: max of the two one-sided bootstrap shares."""
    nb = idx.shape[0]
    bd = d[idx].mean(axis=1)
    return float(max((np.sum(bd <= -margin) + 1) / (nb + 1), (np.sum(bd >= margin) + 1) / (nb + 1)))


def eligible(lk_tool, lk_tc):
    return lk_tc >= LOOKUP_TC_MIN and lk_tool >= LOOKUP_TOOL_MIN


def mh_pairs(a: int, b: int):
    """Pooled matched-pairs Mantel-Haenszel over strata: (exact two-sided p, chi-square p, n discordant)."""
    n = a + b
    if n == 0:
        return 1.0, 1.0, 0
    return sign_p(a, b, True), erfc(sqrt((a - b) ** 2 / n / 2)), n


def _ablated_file(results: Path, tag: str, lab: str) -> str:
    """find_ablated drops the file name; recover it from the cell's own meta (exactly one file must match)."""
    import json
    hit = [f.name for f in sorted(results.glob(f"{E1.PREFIX}-ablated-n*.json"))
           if S0.label(r := json.loads(f.read_text())) == lab and r.get("model_tag") == tag and r.get("arm") == "ablated"]
    assert len(hit) == 1, (tag, lab, hit)
    return hit[0]


def load_cells(results: Path):
    """main[(size, quant, label)] = (ids, contains, looked_up, bucket); abl[...] = (outcome, looked_up) arrays; files."""
    main, abl, files = {}, {}, {}
    for q in E1.QUANTS:
        for s in E1.SIZES:
            t = E1.tag(s, q)
            for lab in ("tool", "tool+contract"):
                reps = S0.find_cells(results, E1.PREFIX, t).get(lab)
                if reps:
                    rows = sorted([a for a in reps[0]["answers"] if a["pass"] == 1], key=lambda a: a["id"])
                    main[(s, q, lab)] = (np.array([a["id"] for a in rows]), np.array([int(a["contains"]) for a in rows], float),
                                         np.array([bool(a["tools"]) for a in rows]))
                    files[(s, q, lab)] = reps[0]["_file"]
            for lab in (ABL_TOOL, ABL_TC):
                reps = S0.find_ablated(results, E1.PREFIX, t).get(lab)
                if reps:
                    reps[0]["_file"] = _ablated_file(results, t, lab)
                    rows = sorted([a for a in reps[0]["answers"] if a["pass"] == 1], key=lambda a: a["id"])
                    abl[(s, q, lab)] = (np.array([a["id"] for a in rows]), np.array([a["outcome"] for a in rows]),
                                        np.array([bool(a["tools"]) for a in rows]))
                    files[(s, q, lab)] = reps[0]["_file"]
    return main, abl, files


def analyse(results: Path):
    """Every number the table prints, as plain data (so tests can check it without parsing markdown)."""
    main, abl, files = load_cells(results)
    ids = main[("4b", "iq4_xs", "tool")][0]
    for k, v in main.items():
        assert (v[0] == ids).all(), f"question ids differ at {k}"
    n, idx = len(ids), E1.boot_idx(len(ids))
    res = {"n": n, "files": files, "acc": {}, "h5a": {}, "h5a_pooled": {}, "abl": {}, "h5b": {}, "h5b_pooled": {}, "h8": {}}

    def ci(v, ix):
        lo, hi = E1.pct(v[ix].mean(axis=1))
        return float(v.mean()), lo, hi

    for (s, q, lab), (_, c, lk) in main.items():
        res["acc"][(s, q, lab)] = {"acc": ci(c, idx), "lookup": ci(lk.astype(float), idx),
                                   "acc_looked": float(c[lk].mean()) if lk.any() else None, "n_looked": int(lk.sum()),
                                   "acc_not_looked": float(c[~lk].mean()) if (~lk).any() else None, "n_not": int((~lk).sum())}
    # H5a
    for q in E1.QUANTS:
        elig = []
        for s in E1.SIZES:
            if (s, q, "tool") in main and (s, q, "tool+contract") in main:
                d = main[(s, q, "tool+contract")][1] - main[(s, q, "tool")][1]
                ltool, ltc = res["acc"][(s, q, "tool")]["lookup"][0], res["acc"][(s, q, "tool+contract")]["lookup"][0]
                ok = eligible(ltool, ltc)
                lo, hi = E1.pct(d[idx].mean(axis=1))
                res["h5a"][(s, q)] = {"diff": float(d.mean()), "lo": lo, "hi": hi, "p": tost_p(d, idx), "eligible": ok,
                                      "ao": int((d > 0).sum()), "bo": int((d < 0).sum())}
                if ok:
                    elig.append(d)
        if elig:
            d = np.mean(elig, axis=0)
            lo, hi = E1.pct(d[idx].mean(axis=1))
            res["h5a_pooled"][q] = {"k": len(elig), "diff": float(d.mean()), "lo": lo, "hi": hi, "p": tost_p(d, idx)}
    # ablated arm
    for (s, q, lab), (aid, oc, lk) in abl.items():
        m = len(oc)
        bi = E1.boot_idx(m, seed=1)
        cell = {"n": m, "lookup": float(lk.mean()), "n_looked": int(lk.sum())}
        for o in ("correct", "abstained", "hallucinated"):
            cell[o] = ci((oc == o).astype(float), bi)
            cell[o + "_looked"] = int(((oc == o) & lk).sum())
            cell[o + "_not"] = int(((oc == o) & ~lk).sum())
        res["abl"][(s, q, lab)] = cell
    for q in E1.QUANTS:
        pa = pb = 0
        for s in E1.SIZES:
            if (s, q, ABL_TOOL) in abl and (s, q, ABL_TC) in abl:
                a_ids, a_oc, _ = abl[(s, q, ABL_TC)]
                b_ids, b_oc, _ = abl[(s, q, ABL_TOOL)]
                assert (a_ids == b_ids).all()
                ha, hb = a_oc == "hallucinated", b_oc == "hallucinated"
                ao, bo = int((ha & ~hb).sum()), int((~ha & hb).sum())
                d = ha.astype(float) - hb.astype(float)
                lo, hi = E1.pct(d[E1.boot_idx(len(d), seed=1)].mean(axis=1))
                res["h5b"][(s, q)] = {"diff": float(d.mean()), "lo": lo, "hi": hi, "ao": ao, "bo": bo, "p": sign_p(ao, bo, True)}
                pa, pb = pa + ao, pb + bo
        res["h5b_pooled"][q] = dict(zip(("p_exact", "p_chi2", "n_disc"), mh_pairs(pa, pb)), tc_more=pa, tool_more=pb)
    # H8
    for q in E1.QUANTS:
        ss = [s for s in E1.SIZES if (s, q, "tool") in main]
        xs = np.log10([E1.PARAMS[s] for s in ss])
        for lab in ("tool", "tool+contract"):
            m = np.array([main[(s, q, lab)][2] for s in ss], float)                 # (sizes, n)
            bt = E1.slope(np.stack([m[:, idx[b]].mean(axis=1) for b in range(idx.shape[0])]), xs)
            res["h8"][(q, lab)] = {"sizes": ss, "slope": float(E1.slope(m.mean(axis=1), xs)), "ci": E1.pct(bt),
                                   "rates": [float(r) for r in m.mean(axis=1)]}
    return res


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def render(results: Path, res: dict) -> list:
    A, nz = res["acc"], res["n"]
    out = ["# H5 (a, b) and H8 — default-prompt cells", "",
           f"Generated by `stats_h5h8.py` from the 36 cells listed at the end (`verify_h5h8.py`: 36 of 36 ok). Main arm: {nz} unambiguous "
           "questions, metric `contains`, 1 repeat (greedy). Ablated arm: first 200 questions, prompt `abstain_default` "
           "(default tool prompt + the abstention sentence, D16). 95% CIs are percentile bootstraps over questions, 10,000 resamples, "
           "one resample shared by every size. Sizes are Qwen3.5 0.8/2/4/9/27B (27B only at IQ4_XS).", ""]

    out += ["## Accuracy and lookup rate, default prompt (`contains`, answerable questions)", "",
            "| quant | size | tool acc [CI] | tool lookup | contract acc [CI] | contract lookup | tool acc if looked up (n) | tool acc if not (n) |",
            "|---|---|---|---|---|---|---|---|"]
    f3 = lambda t: "%.3f [%.3f, %.3f]" % t
    for q in E1.QUANTS:
        for s in E1.SIZES:
            if (s, q, "tool") in A:
                t, c = A[(s, q, "tool")], A[(s, q, "tool+contract")]
                g = lambda x: "–" if x is None else "%.3f" % x
                out.append("| %s | %s | %s | %.3f | %s | %.3f | %s (%d) | %s (%d) |" % (
                    q, s, f3(t["acc"]), t["lookup"][0], f3(c["acc"]), c["lookup"][0],
                    g(t["acc_looked"]), t["n_looked"], g(t["acc_not_looked"]), t["n_not"]))
    out.append("")

    out += ["## H5a — `tool+contract` − `tool` is within ±2 points (TOST), default prompt", "",
            "Eligible = contract lookup ≥ .99 and tool lookup ≥ .8 (D12). Only eligible sizes enter the pooled test; the others are shown "
            "because they are findings. p = max of the two one-sided bootstrap shares (resolution 1/10001 ≈ 1e-4). "
            "Equivalence is **not** shown where p ≥ .05.", "",
            "| quant | size | eligible | Δ acc (contract − tool) [CI] | contract-only | tool-only | TOST p |", "|---|---|---|---|---|---|---|"]
    fp = lambda p: "≤ 1e-4 (resolution)" if p <= 1 / 10001 + 1e-12 else "%.3g" % p
    for (s, q), r in sorted(res["h5a"].items(), key=lambda kv: (E1.QUANTS.index(kv[0][1]), E1.SIZES.index(kv[0][0]))):
        out.append("| %s | %s | %s | %+.3f [%+.3f, %+.3f] | %d | %d | %s |" % (
            q, s, "yes" if r["eligible"] else "no", r["diff"], r["lo"], r["hi"], r["ao"], r["bo"], fp(r["p"])))
    out += ["", "Pooled over the eligible sizes (mean per-question difference):", ""]
    if res["h5a_pooled"]:
        for q, r in res["h5a_pooled"].items():
            out.append("- %s: %d eligible sizes, Δ = %+.3f [%+.3f, %+.3f], TOST p %s." % (q, r["k"], r["diff"], r["lo"], r["hi"], fp(r["p"])))
    for q in E1.QUANTS:
        if q not in res["h5a_pooled"]:
            out.append("- %s: **no eligible size**; the pooled H5a cannot be computed there." % q)
    out.append("")

    out += ["## H5b — hallucination on absent facts, `tool+contract` vs `tool`, default prompt with the abstention clause (D16)", "",
            "Abstention is decided by the regex of `ko/ablate.py` (`label_abstain.py`); " + E1.regex_check(results) +
            " Outcomes: correct (answered from weights) / abstained / hallucinated (a wrong answer).", "",
            "| quant | size | arm | lookup rate | correct [CI] | abstained [CI] | hallucinated [CI] |", "|---|---|---|---|---|---|---|"]
    for q in E1.QUANTS:
        for s in E1.SIZES:
            for lab in (ABL_TOOL, ABL_TC):
                if (s, q, lab) in res["abl"]:
                    r = res["abl"][(s, q, lab)]
                    out.append("| %s | %s | %s | %.3f | %s | %s | %s |" % (q, s, lab.split("/")[0], r["lookup"], f3(r["correct"]),
                                                                           f3(r["abstained"]), f3(r["hallucinated"])))
    out += ["", "Paired difference in hallucination rate (contract − tool, exact two-sided sign test on discordant questions):", "",
            "| quant | size | Δ halluc. [CI] | contract-more | tool-more | p |", "|---|---|---|---|---|---|"]
    for (s, q), r in sorted(res["h5b"].items(), key=lambda kv: (E1.QUANTS.index(kv[0][1]), E1.SIZES.index(kv[0][0]))):
        out.append("| %s | %s | %+.3f [%+.3f, %+.3f] | %d | %d | %.3g |" % (q, s, r["diff"], r["lo"], r["hi"], r["ao"], r["bo"], r["p"]))
    out += ["", "Pooled over sizes (Mantel-Haenszel for matched pairs = sum of the discordants; the same 200 questions occur at every size, "
            "so this p treats dependent sizes as independent strata and is optimistic):", ""]
    for q, r in res["h5b_pooled"].items():
        out.append("- %s: %d discordant (contract more %d, tool more %d); exact p %.3g, chi-square p %.3g." % (
            q, r["n_disc"], r["tc_more"], r["tool_more"], r["p_exact"], r["p_chi2"]))
    out += ["", "### Lookup split (D16: separate forcing from abstaining on doubt)", "",
            "Counts of 200 per arm, split by whether the answer made a tool call. Descriptive, not causal (the model chooses what to look up).", "",
            "| quant | size | arm | looked up | abstained after lookup | abstained **without** lookup | halluc. after lookup | halluc. without | correct after lookup | correct without |",
            "|---|---|---|---|---|---|---|---|---|---|"]
    for q in E1.QUANTS:
        for s in E1.SIZES:
            for lab in (ABL_TOOL, ABL_TC):
                if (s, q, lab) in res["abl"]:
                    r = res["abl"][(s, q, lab)]
                    out.append("| %s | %s | %s | %d | %d | %d | %d | %d | %d | %d |" % (
                        q, s, lab.split("/")[0], r["n_looked"], r["abstained_looked"], r["abstained_not"], r["hallucinated_looked"],
                        r["hallucinated_not"], r["correct_looked"], r["correct_not"]))
    out.append("")
    nowl = [(k, r["abstained_not"] / r["n"]) for k, r in res["abl"].items() if k[2] == ABL_TOOL]
    out.append("D16 check (share of `tool` answers that abstained without a lookup; > 20 % at most sizes would rename H5b): " +
               "; ".join("%s %s %.1f %%" % (k[1], k[0], 100 * v) for k, v in sorted(nowl, key=lambda kv: (E1.QUANTS.index(kv[0][1]), E1.SIZES.index(kv[0][0])))) + ".")
    out.append("")

    out += ["## H8 — lookup rate against log10(parameters), default prompt (exploratory, no p-value, outside Holm)", "",
            "Slope = OLS, lookup-rate points per decade of parameters (rate 0–1). CI: percentile bootstrap over questions. "
            "`tool+contract` is shown for contrast (the contract forces the call, so its slope is not propensity).", "",
            "| quant | arm | sizes | lookup rates | slope per decade | 95% CI |", "|---|---|---|---|---|---|"]
    for (q, lab), r in res["h8"].items():
        out.append("| %s | %s | %s | %s | %+.3f | [%+.3f, %+.3f] |" % (q, lab, ", ".join(r["sizes"]),
                                                                       ", ".join("%.3f" % x for x in r["rates"]), r["slope"], r["ci"][0], r["ci"][1]))
    out += ["", "## Input files (sha256)", "", "| quant | size | arm | file | sha256 |", "|---|---|---|---|---|"]
    for (s, q, lab), f in sorted(res["files"].items(), key=lambda kv: (E1.QUANTS.index(kv[0][1]), E1.SIZES.index(kv[0][0]), kv[0][2])):
        out.append("| %s | %s | %s | `%s` | `%s` |" % (q, s, lab, f, sha256(results / f)))
    out.append("")
    return out


def main():
    results = HERE / "results"
    res = analyse(results)
    out = render(results, res)
    (results / "H5H8_TABLE.md").write_text("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
