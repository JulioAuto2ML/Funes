#!/usr/bin/env python3
"""Did every cell of the GSM8K arm get written, whole and clean? (design §8)

  python3 verify_gsm8k.py [--results DIR] [--with-tool]    # exit 0 only if every expected cell checks out

Expected: IQ4_XS 0.8/2/4/9/27B and Q8_0 0.8/2/4/9B x (closed, store) = 18 cells; with --with-tool also the 5 IQ4_XS `tool` cells.
Each: 500 answers = the n=500 sample (same ids in every cell), 0 errors, dataset sha256 equal to the pinned one, model hash verified,
one `max_tokens` across all cells. Matched by the file's own `model_tag` and `condition`, never by name; pilot files never count.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ko import gsm8k as G  # noqa: E402

IQ = ["qwen3.5-0.8b-iq4_xs", "qwen3.5-2b-iq4_xs", "qwen3.5-4b-iq4_xs", "qwen3.5-9b-iq4_xs", "qwen3.5-27b-iq4_xs"]
Q8 = ["qwen3.5-0.8b-q8_0", "qwen3.5-2b-q8_0", "qwen3.5-4b-q8_0", "qwen3.5-9b-q8_0"]


def expected(with_tool: bool) -> list[tuple[str, str]]:
    cells = [(t, c) for t in IQ + Q8 for c in G.H2_CONDITIONS]
    return cells + ([(t, "tool") for t in IQ] if with_tool else [])


def check(results: Path, with_tool: bool = False, pinned: str | None = None):
    pinned = pinned or G.EXPECTED_SHA256
    found = {}
    for f in sorted(results.glob("gsm8k-n*-r0.json")):
        r = json.loads(f.read_text())
        if r.get("pilot"):
            continue
        found[(r.get("model_tag"), r.get("condition"))] = (f, r)
    rows, bad, ids_ref, max_tokens = [], 0, None, set()
    for key in expected(with_tool):
        hit = found.get(key)
        if not hit:
            rows.append((*key, "MISSING", "")); bad += 1
            continue
        f, r = hit
        a = r["answers"]
        why = []
        if len(a) != G.N or r.get("n_questions") != G.N:
            why.append(f"{len(a)} answers (n_questions {r.get('n_questions')}), expected {G.N}")
        errs = sum(1 for x in a if x.get("error"))
        if errs:
            why.append(f"{errs} answers with an error")
        if r.get("dataset_sha256") != pinned:
            why.append("dataset sha256 is not the pinned one")
        if (r.get("model") or {}).get("verified") != "sha256":
            why.append(f"model hash not verified ({(r.get('model') or {}).get('verified')})")
        ids = [x["id"] for x in a]
        if ids_ref is None:
            ids_ref = ids
        elif ids != ids_ref:
            why.append("not the same problems as the other cells")
        max_tokens.add(r.get("max_tokens"))
        rows.append((*key, "ok" if not why else "BAD", "; ".join(why) or f"{len(a)} answers, {f.name}"))
        bad += bool(why)
    if len(max_tokens) > 1:
        rows.append(("-", "-", "BAD", f"more than one max_tokens across cells: {sorted(map(str, max_tokens))}")); bad += 1
    return rows, bad


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=str(HERE / "results"))
    ap.add_argument("--with-tool", action="store_true")
    a = ap.parse_args(argv)
    rows, bad = check(Path(a.results), a.with_tool)
    for r in rows:
        print(f"{r[0]:<22} {r[1]:<8} {r[2]:<8} {r[3]}")
    print(f"\n{sum(r[2] == 'ok' for r in rows)} of {len(rows)} cells ok; {bad} missing or bad")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
