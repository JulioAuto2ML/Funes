"""stats_funes: the verification gate and the paired table, on synthetic cells (no model, no network)."""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import stats_funes as F  # noqa: E402


def mk(size, cond, hits, overrides=None):
    c = {"model": {"id": f"qwen3.5-{size}-iq4_xs", "verified": "sha256"}, "adapters": [], "funes_pool": 14267, "wall_s": 1.0,
         "answers": [{"id": str(i), "contains": int(h), "pop_bucket": f"1e{1 + i % 5}", "error": None} for i, h in enumerate(hits)],
         "summary": {"pass1": {"lookup_rate": 0.1, "tainted": 0.0}}}
    c.update(overrides or {})
    return c


def write(tmp, kw=None):
    kw = kw or {}
    for s in F.SIZES:
        for cond in ("funes", "store"):
            tail = "funes-pool14267" if cond == "funes" else cond
            c = kw.get((s, cond)) or mk(s, cond, [1] * 1000 if cond == "store" else [1] * 990 + [0] * 10)
            (tmp / f"popqa-stratified1u-n1000-qwen3.5-{s}-iq4_xs-{tail}-r0.json").write_text(json.dumps(c))


def test_table_reports_paired_difference(tmp_path):
    write(tmp_path)
    text = F.report(tmp_path)
    assert "-0.010" in text and "| 0.8b | 1.000 | 0.990 |" in text


def test_a_cell_with_an_adapter_is_refused(tmp_path):
    write(tmp_path, {("2b", "funes"): mk("2b", "funes", [1] * 1000, {"adapters": [{"id": "x"}]})})
    with pytest.raises(SystemExit):
        F.report(tmp_path)


def test_different_question_sets_are_refused(tmp_path):
    other = mk("4b", "funes", [1] * 1000)
    other["answers"][0]["id"] = "zzz"
    write(tmp_path, {("4b", "funes"): other})
    with pytest.raises(SystemExit):
        F.report(tmp_path)


def test_unverified_model_is_refused(tmp_path):
    write(tmp_path, {("9b", "store"): mk("9b", "store", [1] * 1000, {"model": {"id": "qwen3.5-9b-iq4_xs", "verified": None}})})
    with pytest.raises(SystemExit):
        F.report(tmp_path)
