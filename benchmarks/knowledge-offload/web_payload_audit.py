#!/usr/bin/env python3
"""What the web cells' search results contained: gold in the model's view, and what the model did with it.

  python3 web_payload_audit.py --payloads DIR     # DIR = a copy of yoda's results/payloads  -> results/WEB_AUDIT.md

No model, no network. Payloads (gitignored, on yoda) hold the raw reply of every search. The *model's view* of a
search is rebuilt with the harness's own formatting (`ko/tools.py`: first 5 hits, `title | url | excerpts[:600]`);
the *full view* is every hit's whole excerpt text. Gold is "in view" when one accepted alias occurs in the
normalised text with word boundaries (the `contains` rule of `ko/scoring.py`). Answers are scored by the result
file's own `contains`. Everything is per question, measured; a reading of why is not made here.
"""
import argparse
import gzip
import json
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ko import popqa                                                    # noqa: E402
from ko.scoring import normalize                                        # noqa: E402

PREFIX = "popqa-stratified1u-n1000"


def views(payload_file: Path) -> tuple[str, str, int, list[str], int]:
    """(model view text, full text, number of searches, queries, searches that returned no result list) of one question."""
    d = json.loads(gzip.open(payload_file).read())
    model, full, queries, failed = [], [], [], 0
    for p in d["payloads"]:
        if p["kind"] != "search":
            continue
        raws = [a["raw"] for a in p["attempts"] if "raw" in a]
        queries.append(p["query"])
        if not raws:
            continue
        text = raws[-1].strip()
        if not text.startswith("{"):
            text = next(l[5:] for l in text.splitlines() if l.startswith("data:") and l[5:].strip().startswith("{"))
        res = json.loads(text).get("result", {})
        body = "".join(c.get("text", "") for c in res.get("content", []) if c.get("type") == "text")
        try:
            hits = json.loads(body).get("results", [])
        except ValueError:                   # e.g. "Error executing tool web_search: 402: Insufficient credit ..."
            failed += 1
            model.append(body[:300])
            continue
        for h in hits[:5]:
            title = " ".join((h.get("title") or "").split())
            exc = " … ".join(" ".join(e.split()) for e in (h.get("excerpts") or []) if isinstance(e, str))
            if title and h.get("url"):
                model.append(f"{title} | {h['url']} | {exc[:600]}")
        for h in hits:
            full.append(" ".join(" ".join(e.split()) for e in (h.get("excerpts") or []) if isinstance(e, str)))
    return "\n".join(model), "\n".join(full), len(queries), queries, failed


def has_gold(text: str, aliases) -> bool:
    padded = f" {normalize(text)} "
    return any(f" {a} " in padded for a in (normalize(x) for x in aliases) if a)


def audit(result_file: Path, pdir: Path, items: dict, p: int = 1) -> dict:
    r = json.loads(result_file.read_text())
    rows = {a["id"]: a for a in r["answers"] if a["pass"] == p}
    cell = Counter()
    ex = []
    for i, a in rows.items():
        f = pdir / f"{i}.json.gz"
        searched = f.exists()
        c = bool(a["contains"])
        if not searched:
            cell["unsearched", c] += 1
            continue
        mv, fv, nq, qs, nfail = views(f)
        if nfail:
            cell["failed", c] += 1
            cell["failed_searches"] += nfail
            cell["searches_all"] += nq
            continue
        cell["searches_all"] += nq
        g_m, g_f = has_gold(mv, items[i].aliases), has_gold(fv, items[i].aliases)
        cell["searched", g_m, c] += 1
        cell["full", g_f, g_m] += 1
        cell["nsearch", min(nq, 4)] += 1
        if g_m and not c and len(ex) < 8:
            ex.append((i, items[i].question, items[i].obj, a["prediction"][:100], qs[0]))
    cell[("n",)] = len(rows)
    return {"cell": cell, "examples": ex, "rows": rows}


def table(name: str, res: dict) -> list[str]:
    c = res["cell"]
    n = c[("n",)]
    s = sum(v for k, v in c.items() if k[0] == "searched")
    f0, f1 = c["failed", False], c["failed", True]
    u0, u1 = c["unsearched", False], c["unsearched", True]
    out = [f"### {name}", "", f"{n} questions; not searched {u0 + u1} (accuracy {u1 / max(1, u0 + u1):.3f}, {u1}/{u0 + u1}); searched with every search returning results {s}; "
           f"searched with at least one search that returned an error instead of results (Parallel 402 'Insufficient credit') {f0 + f1} "
           f"(accuracy {f1 / max(1, f0 + f1):.3f}, {f1}/{f0 + f1}); failed searches {c['failed_searches']} of {c['searches_all']} searches.", "",
           "Rows below cover only the questions whose searches all returned results.", "",
           "| searched questions | answer correct | answer wrong | total | accuracy |", "|---|---|---|---|---|"]
    for g, lab in ((True, "gold in the model's view"), (False, "gold not in the model's view")):
        a, b = c["searched", g, True], c["searched", g, False]
        out.append(f"| {lab} | {a} | {b} | {a + b} | {a / max(1, a + b):.3f} |")
    gm = c["searched", True, True] + c["searched", True, False]
    out += ["", f"Gold in the model's view for {gm}/{s} searched questions ({gm / max(1, s):.3f}); of those the answer was wrong for "
            f"{c['searched', True, False]} ({c['searched', True, False] / max(1, gm):.3f}). Answers correct without gold in view: "
            f"{c['searched', False, True]} ({c['searched', False, True] / max(1, s - gm):.3f} of {s - gm}).",
            "Gold in the *full* excerpts but cut by the 600-character limit: %d; in neither: %d; (full view, model view): %s." % (
                c["full", True, False], c["full", False, False],
                ", ".join(f"{k[1:]}={v}" for k, v in sorted(c.items(), key=str) if k[0] == "full")),
            "Searches per searched question (1, 2, 3, 4+): " + ", ".join(str(c["nsearch", k]) for k in (1, 2, 3, 4)) + ".", ""]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--payloads", required=True)
    ap.add_argument("--results", default=str(HERE / "results"))
    ap.add_argument("--out", default=str(HERE / "results" / "WEB_AUDIT.md"))
    args = ap.parse_args()
    items = {i.id: i for i in popqa.load(popqa.DEFAULT_PATH)}
    results, pay = Path(args.results), Path(args.payloads)
    out = ["# Web cells: what the search results held (E1, Qwen3.5 IQ4_XS, 1000 stratified unambiguous items, prompt search_first)", "",
           "Generated by `web_payload_audit.py` from the raw payloads. Model's view = the harness's own formatting of each search "
           "(5 hits, excerpts cut at 600 characters). No explanation is attempted; see the notebook for what is hypothesis.", ""]
    summary = {}
    for size in ("0.8b", "9b", "27b"):
        t = f"qwen3.5-{size}-iq4_xs"
        res = audit(results / f"{PREFIX}-{t}-web-search_first-r0.json", pay / f"{t}-stratified1u" / "web-search_first-r0-p1", items)
        summary[size] = res
        out += table(f"web, {size}", res)
        if res["examples"]:
            out += ["Wrong answers with gold in the model's view (first 8; id, question, gold, prediction, first query):", ""]
            out += [f"- {e[0]} | {e[1]} | {e[2]} | {e[3]!r} | {e[4]!r}" for e in res["examples"]] + [""]
    # same question across sizes
    ids = sorted(set.intersection(*(set(summary[s]["rows"]) for s in summary)))
    out += ["### Same questions across sizes (web)", "", "| size | searched | correct | accuracy on the questions this size searched | accuracy on those it did not |",
            "|---|---|---|---|---|"]
    for s, res in summary.items():
        rows = res["rows"]
        srch = [i for i in ids if (pay / f"qwen3.5-{s}-iq4_xs-stratified1u" / "web-search_first-r0-p1" / f"{i}.json.gz").exists()]
        non = [i for i in ids if i not in set(srch)]
        out.append("| %s | %d | %d | %.3f | %s |" % (s, len(srch), sum(rows[i]["contains"] for i in ids),
                   sum(rows[i]["contains"] for i in srch) / max(1, len(srch)),
                   "%.3f (%d)" % (sum(rows[i]["contains"] for i in non) / len(non), len(non)) if non else "-"))
    s9 = {i for i in ids if (pay / "qwen3.5-9b-iq4_xs-stratified1u/web-search_first-r0-p1" / f"{i}.json.gz").exists()}
    s27 = {i for i in ids if (pay / "qwen3.5-27b-iq4_xs-stratified1u/web-search_first-r0-p1" / f"{i}.json.gz").exists()}
    both = sorted(s9 & s27)
    r9, r27 = summary["9b"]["rows"], summary["27b"]["rows"]
    out += ["", "Questions both 9B and 27B searched: %d: 9B %.3f, 27B %.3f. Searched by 27B only: %d (9B %.3f, 27B %.3f). Searched by 9B only: %d." % (
        len(both), sum(r9[i]["contains"] for i in both) / len(both), sum(r27[i]["contains"] for i in both) / len(both),
        len(s27 - s9), sum(r9[i]["contains"] for i in s27 - s9) / max(1, len(s27 - s9)), sum(r27[i]["contains"] for i in s27 - s9) / max(1, len(s27 - s9)),
        len(s9 - s27)), ""]
    # gold in view on the questions both searched, per size
    both_gold = {}
    for s in ("9b", "27b"):
        d = pay / f"qwen3.5-{s}-iq4_xs-stratified1u/web-search_first-r0-p1"
        both_gold[s] = {i: has_gold(views(d / f"{i}.json.gz")[0], items[i].aliases) for i in both}
    out += ["On those %d questions: gold in the model's view for 9B %d, for 27B %d; the two searched with the same first query on %d of them." % (
        len(both), sum(both_gold["9b"].values()), sum(both_gold["27b"].values()),
        sum(views(pay / f"qwen3.5-9b-iq4_xs-stratified1u/web-search_first-r0-p1/{i}.json.gz")[3][0] ==
            views(pay / f"qwen3.5-27b-iq4_xs-stratified1u/web-search_first-r0-p1/{i}.json.gz")[3][0] for i in both)), ""]

    # every web cell's search errors, from the raw payloads, for ko/validity.py (summarize and stats_e1 read this file)
    audit_json = {}
    for f in sorted(results.glob(f"{PREFIX}-*-web*-search_first-r0.json")):
        r = json.loads(f.read_text())
        stem = f.name.split(r["model_tag"] + "-", 1)[1].removesuffix(".json")
        nf = ns = 0
        for p in (1, 2):
            d = pay / (r["model_tag"] + "-stratified1u") / f"{stem}-p{p}"
            for g in (d.glob("*.json.gz") if d.exists() else []):
                v = views(g)
                nf += v[4]
                ns += v[2]
        audit_json[f.name] = {"failed_searches": nf, "searches": ns, "source": "web_payload_audit.py",
                              "reason": "Parallel 402 Insufficient credit in the raw payloads"}
    (results / "search_audit.json").write_text(json.dumps(audit_json, indent=1, sort_keys=True) + "\n")
    out += ["### Search errors per web cell (written to `results/search_audit.json`)", ""] + [
        f"- `{k}`: {v['failed_searches']} of {v['searches']} searches failed" for k, v in audit_json.items()] + [""]
    # the 27B's failed searches: where in the sample they start, and the clean subset against the 9B
    d27 = pay / "qwen3.5-27b-iq4_xs-stratified1u" / "web-search_first-r0-p1"
    order = [x["id"] for x in json.loads((results / f"{PREFIX}-qwen3.5-27b-iq4_xs-web-search_first-r0.json").read_text())["answers"]]
    fails = {i: views(d27 / f"{i}.json.gz")[4] for i in order if (d27 / f"{i}.json.gz").exists()}
    first = next((k for k, i in enumerate(order) if fails.get(i)), None)
    clean = [i for i, f in fails.items() if not f]
    out += ["### 27B: searches that failed with a credit error", "",
            "First question (position in the sample order) whose search failed: %s of %d. Searched questions before it: %d, all clean. "
            "Searched questions from it on: %d, with a clean one among them: %d." % (
                first, len(order), sum(1 for i in order[:first] if i in fails),
                sum(1 for i in order[first:] if i in fails), sum(1 for i in order[first:] if i in fails and not fails[i])), "",
            "On the %d questions the 27B searched with every search clean: 27B accuracy %.3f, 9B accuracy %.3f (9B's own cell, whether or not it searched)." % (
                len(clean), sum(summary["27b"]["rows"][i]["contains"] for i in clean) / len(clean),
                sum(summary["9b"]["rows"][i]["contains"] for i in clean) / len(clean)), ""]
    # writeback vs web, 9B
    t = "qwen3.5-9b-iq4_xs"
    web = json.loads((results / f"{PREFIX}-{t}-web-search_first-r0.json").read_text())
    wb = json.loads((results / f"{PREFIX}-{t}-webpluswriteback-search_first-r0.json").read_text())
    out += ["### web+writeback vs web (9B, same questions)", ""]
    for p in (1, 2):
        w = {a["id"]: a for a in web["answers"]}
        b = {a["id"]: a for a in wb["answers"] if a["pass"] == p}
        grp = Counter()
        for i, a in b.items():
            ws = "web_search" in w[i]["tools"]
            bs = "web_search" in a["tools"]
            br = "recall" in a["tools"]
            grp[(ws, bs, br, "web", bool(w[i]["contains"]))] += 1
            grp[(ws, bs, br, "wb", bool(a["contains"]))] += 1
            grp[(ws, bs, br, "n")] += 1
        out += [f"Pass {p}. Groups by (web searched, writeback searched, writeback recalled): n, web accuracy, writeback accuracy.", "",
                "| web searched | wb searched | wb recalled | n | web acc | wb acc |", "|---|---|---|---|---|---|"]
        for k in sorted({k[:3] for k in grp}, reverse=True):
            n = grp[k + ("n",)]
            out.append("| %s | %s | %s | %d | %.3f | %.3f |" % (*k, n, grp[k + ("web", True)] / n, grp[k + ("wb", True)] / n))
        order = Counter()
        for a in b.values():
            ts = a["tools"]
            order[(ts[0] if ts else "none")] += 1
        out += ["", f"First tool called in pass {p}: " + ", ".join(f"{k} {v}" for k, v in order.most_common()) + ".", ""]
    # writeback payloads: searched questions' gold in view
    for p in (1, 2):
        d = pay / f"{t}-stratified1u/webpluswriteback-search_first-r0-p{p}"
        res = audit(results / f"{PREFIX}-{t}-webpluswriteback-search_first-r0.json", d, items, p=p)
        out += table(f"web+writeback pass {p}, 9B", res)
    Path(args.out).write_text("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    main()
