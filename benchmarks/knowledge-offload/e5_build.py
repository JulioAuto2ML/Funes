#!/usr/bin/env python3
"""E5 data builder (E5_DESIGN.md section 3). Offline: no model, no GPU, no network.

  python3 e5_build.py --out DIR                        lookup L1/L0 data, dev set, held-out draw, manifest
  python3 e5_build.py --out DIR --n-lookup 3000        the retrain size
  ... --anchor-prompts train.jsonl --anchor-answers base_answers.jsonl --n-anchor 500
  ... --templates e5_paraphrase_templates.json --templates-sha256 <recorded hash>

Same inputs, same bytes: the sha256 of every file is printed and written to manifest.json.
"""
import argparse
import sys
from pathlib import Path

from ko import e5_data as D, e5_paraphrase as P, gsm8k, popqa


def default_n_anchor(n_lookup: int) -> int:
    """ANCHOR_RATIO applies to the REQUESTED lookup count (--n-lookup), not to the number written after
    ungrounded examples are dropped: 1000 -> 500, 3000 -> 1500."""
    return int(n_lookup * D.ANCHOR_RATIO)


def add_anchors(a, out: Path, log=print) -> None:
    stats = {}
    rows = D.build_anchor_rows(Path(a.anchor_prompts), Path(a.anchor_answers), a.n_anchor,
                               test_path=gsm8k.DEFAULT_PATH, stats=stats)
    log(f"anchors: {stats}")
    if stats["dropped_length"]:
        log(f"anchors: {stats['dropped_length']} base answers dropped (finish_reason=length)")
    hashes = {}
    for arm, r in rows.items():
        hashes[f"anchors_{arm}.jsonl"] = D.write_jsonl(out / f"anchors_{arm}.jsonl", r)
        log(f"anchors_{arm}.jsonl  {hashes[f'anchors_{arm}.jsonl']}  ({len(r)} rows)")
    name = "anchors_manifest.json"
    log(f"{name}  {D.write_json(out / name, {**stats, 'n_requested': a.n_anchor, 'files_sha256': hashes})}")


def add_paraphrases(a, out: Path, sample, log=print) -> None:
    templates = P.load_templates(Path(a.templates), a.templates_sha256)
    log(P.describe_templates(sample, templates))
    rows = P.apply_templates(P.paraphrase_items(sample), templates)
    log(f"paraphrase_P.jsonl  {D.write_jsonl(out / 'paraphrase_P.jsonl', rows)}  "
        f"({len(rows)} rows, templates sha256 {templates.sha256})")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--data", default=str(popqa.DEFAULT_PATH))
    ap.add_argument("--n-lookup", type=int, default=D.N_LOOKUP_START)
    ap.add_argument("--anchor-prompts")
    ap.add_argument("--anchor-answers")
    ap.add_argument("--n-anchor", type=int, default=None,
                    help="default: n-lookup * ANCHOR_RATIO (0.5) of the REQUESTED --n-lookup (500 for 1000, "
                         "1500 for 3000), not of the number written")
    ap.add_argument("--templates")
    ap.add_argument("--templates-sha256")
    a = ap.parse_args(argv)
    if bool(a.anchor_prompts) != bool(a.anchor_answers):
        ap.error("--anchor-prompts and --anchor-answers go together")
    if a.n_anchor is None:
        a.n_anchor = default_n_anchor(a.n_lookup)
    data = Path(a.data)
    if not data.exists():
        sys.exit(f"{data} missing: run `run.py fetch`")
    try:
        out = Path(a.out)
        D.build(data, out, a.n_lookup)
        if a.anchor_prompts:
            add_anchors(a, out)
        if a.templates:
            add_paraphrases(a, out, D.eval_items(popqa.load(data)))
    except D.E5DataError as e:
        sys.exit(f"e5_build: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
