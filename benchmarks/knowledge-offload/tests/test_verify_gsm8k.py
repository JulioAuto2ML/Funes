import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

import verify_gsm8k as V
from ko import gsm8k as G

PIN = "ab" * 32


def cell(tag, cond, n=500, err=0, sha=PIN, verified="sha256", mt=512, pilot=False, ids=None):
    ids = ids or [f"p{i}" for i in range(n)]
    return {"model_tag": tag, "condition": cond, "dataset_sha256": sha, "model": {"verified": verified}, "pilot": pilot,
            "max_tokens": mt, "n_questions": n, "answers": [{"id": i, "error": "x" if k < err else None} for k, i in enumerate(ids)]}


def write_all(d, over=None):
    over = over or {}
    for tag, cond in V.expected(False):
        c = cell(tag, cond)
        c.update(over.get((tag, cond), {}))
        (d / f"gsm8k-n500-s1-{tag}-{cond}-r0.json").write_text(json.dumps(c))


def test_all_good(tmp_path):
    write_all(tmp_path)
    rows, bad = V.check(tmp_path, pinned=PIN)
    assert bad == 0 and len(rows) == 18


def test_missing_cell_is_reported_not_crashed(tmp_path):
    write_all(tmp_path)
    next(tmp_path.glob("*27b-iq4_xs-store*")).unlink()
    rows, bad = V.check(tmp_path, pinned=PIN)
    assert bad == 1 and any(r[2] == "MISSING" for r in rows)


def test_error_hash_and_count_failures(tmp_path):
    write_all(tmp_path, {("qwen3.5-9b-iq4_xs", "closed"): {"dataset_sha256": "0" * 64},
                           ("qwen3.5-4b-iq4_xs", "store"): {"answers": cell("a", "b", err=2)["answers"]},
                           ("qwen3.5-2b-q8_0", "closed"): {"answers": cell("a", "b", n=10)["answers"], "n_questions": 10}})
    rows, bad = V.check(tmp_path, pinned=PIN)
    assert bad == 3


def test_pilot_files_never_count(tmp_path):
    write_all(tmp_path)
    p = tmp_path / "gsm8k-n20-s2-x-closed-r0.json"
    p.write_text(json.dumps(cell("qwen3.5-9b-iq4_xs", "closed", n=20, pilot=True)))
    rows, bad = V.check(tmp_path, pinned=PIN)
    assert bad == 0


def test_mixed_max_tokens_is_bad(tmp_path):
    write_all(tmp_path, {("qwen3.5-9b-iq4_xs", "closed"): {"max_tokens": 1024}})
    rows, bad = V.check(tmp_path, pinned=PIN)
    assert bad >= 1


def test_different_problems_is_bad(tmp_path):
    write_all(tmp_path, {("qwen3.5-9b-iq4_xs", "closed"): {"answers": cell("a", "b", ids=[f"z{i}" for i in range(500)])["answers"]}})
    rows, bad = V.check(tmp_path, pinned=PIN)
    assert bad == 1


def test_with_tool_expects_five_more():
    assert len(V.expected(True)) == 23
