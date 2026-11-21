#!/usr/bin/env python3
"""Did every E5 cell of e5_eval.cells() get written, whole and clean, by the adapter it names?

  python3 verify_e5.py [--results DIR] [--p-file paraphrase_P.jsonl]    # exit 0 only if every cell checks out

Each cell is found by its own meta (model_tag, task, condition, prompt variant, arm, paraphrase), never by file name, and
checked: the expected number of answers, 0 answers with an `error`, the served model sha256-verified and the planned base,
the applied adapter in `adapters` exactly the planned one (scale 1.0, sha256-verified, `transfer` true only on the Q8_0
row), the E1 sample for PopQA cells (stratified, seed 1, ambiguous excluded), GSM8K's pinned data hash, and for set P the
file's hash and the first 200 sample ids in order. A cell found twice is BAD. This runs before any statistic.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import e5_eval  # noqa: E402


def key_of(r: dict) -> tuple:
    gs = r.get("dataset") == "gsm8k-test"
    return (r.get("model_tag"), "gsm8k" if gs else "popqa", r.get("condition"),
            "default" if gs else r.get("prompt_variant", "default"), "main" if gs else r.get("arm", "main"),
            bool(r.get("paraphrase")))


def cell_key(c: e5_eval.Cell) -> tuple:
    return (c.tag, c.task, c.condition, c.variant, c.arm, c.paraphrase)


def load(results: Path) -> dict:
    found: dict = {}
    for f in sorted(results.glob("*.json")):
        if not f.name.startswith(("popqa-", "gsm8k-")) or f.name.startswith(("gsm8k-pilot", "popqa-n")):
            continue
        try:
            r = json.loads(f.read_text())
        except json.JSONDecodeError:
            continue
        if isinstance(r, dict) and "answers" in r and (r.get("adapters") or "-l" in str(r.get("model_tag", ""))):
            found.setdefault(key_of(r), []).append((f, r))
    return found


def check_cell(c: e5_eval.Cell, f: Path, r: dict, p_ids: list[str] | None) -> list[str]:
    a = [x for x in r["answers"] if x["pass"] == 1]
    why = []
    if len(a) != c.n or r.get("n_questions") != c.n:
        why.append(f"{len(a)} answers (n_questions {r.get('n_questions')}), expected {c.n}")
    errs = sum(1 for x in a if x.get("error"))
    if errs:
        why.append(f"{errs} answers with an error")
    m = r.get("model") or {}
    if m.get("verified") != "sha256":
        why.append(f"model not sha256-verified ({m.get('verified')})")
    if m.get("id") != c.base:
        why.append(f"served {m.get('id')}, planned {c.base}")
    ad = r.get("adapters") or []
    if [x.get("id") for x in ad] != [c.adapter]:
        why.append(f"adapters {[x.get('id') for x in ad]}, planned [{c.adapter}]")
    else:
        if ad[0].get("verified") != "sha256" or float(ad[0].get("scale", 0)) != 1.0:
            why.append(f"adapter not sha256-verified at scale 1.0 ({ad[0].get('verified')}, {ad[0].get('scale')})")
        want_transfer = ad[0]["base"] != c.base
        if bool(ad[0].get("transfer")) != want_transfer:
            why.append("transfer flag does not match the adapter's own base")
    if c.task == "popqa":
        if r.get("sample") != "stratified" or r.get("dataset_seed") != 1 or not r.get("exclude_ambiguous"):
            why.append("not the E1 sample (stratified, seed 1, unambiguous)")
        if c.paraphrase:
            p = r.get("paraphrase") or {}
            if not p.get("sha256") or p.get("n") != e5_eval.P_N:
                why.append(f"set P not recorded in meta ({p})")
            if p_ids is not None and [x["id"] for x in a] != p_ids:
                why.append("answer ids are not the first 200 sample ids in order")
    else:
        if r.get("dataset_seed") != 1 or r.get("pilot"):
            why.append("not the GSM8K n=500 seed-1 sample")
    return why


def check(results: Path, p_file: Path | None = None) -> tuple[list[tuple], int]:
    found = load(results)
    p_ids = None
    p_sha = set()
    if p_file and p_file.exists():
        p_ids = [json.loads(l)["id"] for l in p_file.read_text().splitlines() if l.strip()]
    rows, bad = [], 0
    for c in e5_eval.cells():
        label = f"{c.tag} {c.task}/{c.condition}" + (f"/{c.variant}" if c.variant != "default" else "") \
            + (" ablated" if c.arm == "ablated" else "") + (" P" if c.paraphrase else "")
        hits = found.get(cell_key(c), [])
        if not hits:
            rows.append((label, "MISSING", "")); bad += 1
            continue
        if len(hits) > 1:
            rows.append((label, "BAD", f"{len(hits)} files for one cell: {[h[0].name for h in hits]}")); bad += 1
            continue
        f, r = hits[0]
        why = check_cell(c, f, r, p_ids)
        if c.paraphrase and (r.get("paraphrase") or {}).get("sha256"):
            p_sha.add(r["paraphrase"]["sha256"])
        if why:
            bad += 1
        rows.append((label, "BAD" if why else "ok", "; ".join(why) or f"{c.n} answers, {f.name}"))
    if len(p_sha) > 1:
        rows.append(("set P", "BAD", f"cells were run on different P files: {sorted(s[:8] for s in p_sha)}")); bad += 1
    return rows, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", default=str(HERE / "results"))
    ap.add_argument("--p-file", default=str(HERE / "data/e5/build_n1000/paraphrase_P.jsonl"))
    a = ap.parse_args(argv)
    rows, bad = check(Path(a.results), Path(a.p_file))
    for r in rows:
        print("%-62s %-8s %s" % r)
    ok = sum(1 for r in rows if r[1] == "ok")
    print(f"\n{ok} of {len(e5_eval.cells())} cells ok; {bad} missing or bad")
    return 0 if bad == 0 and ok == len(e5_eval.cells()) else 1


if __name__ == "__main__":
    sys.exit(main())
