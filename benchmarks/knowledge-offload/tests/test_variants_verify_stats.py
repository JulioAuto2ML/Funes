"""verify_variants.py (12 cells present, whole and clean) and stats_variants.py (reading table, runs on missing cells)."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import stats_variants as S   # noqa: E402
import verify_variants as V  # noqa: E402


def cell(d, tag, var, n=1000, errors=0, verified="sha256", lookups=0, contains=None, ctoks=5, cond="tool"):
    name = f"popqa-stratified1u-n{n}-{tag}-{cond}-{var}-r0.json" if var != "default" else f"popqa-stratified1u-n{n}-{tag}-{cond}-r0.json"
    ans = [{"id": str(i), "pass": 1, "error": "x" if i < errors else "", "tools": ["lookup"] if i < lookups else [],
            "contains": bool(contains(i)) if contains else False, "prediction": "" if i == 0 else "ans",
            "usage": {"completion_tokens": ctoks, "calls": 1}} for i in range(n)]
    (d / name).write_text(json.dumps({"model_tag": tag, "condition": cond, "prompt_variant": var, "arm": "main", "n_questions": n,
                                      "answers": ans, "sample": "stratified", "dataset_seed": 1, "exclude_ambiguous": True,
                                      "model": {"verified": verified}, "wall_s": 12.0}))


def full(d):
    for tag in V.TAGS:
        for var in V.VARIANTS:
            cell(d, tag, var)


def test_all_12_cells_present_and_clean(tmp_path):
    full(tmp_path)
    rows, bad = V.check(tmp_path)
    assert len(rows) == 12 and bad == 0 and all(r[3] == "ok" for r in rows)


def test_expected_plan_is_stage_a_then_b():
    assert V.TAGS == ["qwen3.5-9b-iq4_xs", "qwen3.5-4b-iq4_xs", "qwen3.5-4b-q8_0", "qwen3.5-9b-q8_0"]
    assert V.VARIANTS == ["tool_first", "tool_free", "tool_describe"]


def test_missing_short_errored_and_unverified_are_flagged(tmp_path):
    full(tmp_path)
    (tmp_path / "popqa-stratified1u-n1000-qwen3.5-9b-iq4_xs-tool-tool_first-r0.json").unlink()
    cell(tmp_path, "qwen3.5-4b-iq4_xs", "tool_free", n=999)
    cell(tmp_path, "qwen3.5-4b-q8_0", "tool_describe", errors=2)
    cell(tmp_path, "qwen3.5-9b-q8_0", "tool_first", verified="name-only")
    rows, bad = V.check(tmp_path)
    st = {(r[0], r[1]): r for r in rows}
    assert st[("qwen3.5-9b-iq4_xs", "tool_first")][3] == "MISSING"
    assert "999 answers" in st[("qwen3.5-4b-iq4_xs", "tool_free")][4]
    assert "2 answers with an error" in st[("qwen3.5-4b-q8_0", "tool_describe")][4]
    assert "not sha256-verified" in st[("qwen3.5-9b-q8_0", "tool_first")][4]
    assert bad == 4


def test_the_baseline_default_cell_does_not_count_as_a_variant(tmp_path):
    for tag in V.TAGS:
        cell(tmp_path, tag, "default")
    rows, bad = V.check(tmp_path)
    assert bad == 12 and all(r[3] == "MISSING" for r in rows)


def test_stats_run_on_missing_cells_and_say_so(tmp_path):
    cell(tmp_path, "qwen3.5-9b-iq4_xs", "default", lookups=1, contains=lambda i: i % 2 == 0)
    text = "\n".join(S.render(tmp_path, nb=200))
    assert text.count("missing") >= 11 and "R1" in text and "R5" in text


def test_stats_numbers_on_a_small_known_case(tmp_path):
    tag = "qwen3.5-9b-iq4_xs"
    cell(tmp_path, tag, "default", lookups=0, contains=lambda i: i < 300)
    cell(tmp_path, tag, "tool_free", lookups=600, contains=lambda i: i < 700, ctoks=130)
    r = S.analyse(tmp_path, nb=500)[(tag, "tool_free")]
    assert r["lookup"] == 0.6 and r["acc"] == 0.7 and r["n"] == 1000
    # looked up = ids 0-599, all correct (ids < 700); not looked up = ids 600-999, 100 of 400 correct
    assert r["acc_looked"] == 1.0 and r["n_looked"] == 600 and r["n_not"] == 400 and r["acc_not"] == 0.25
    assert r["limit_share"] == 1.0 and r["limit_flag"] is True
    assert r["lookup_ci"][0] < 0.6 < r["lookup_ci"][1]
    assert r["agree_with_base"] == 0.4        # base never looked up; variant looked up 600 of 1000 -> 400 agree
    assert r["empty"] == 1


def test_a_cell_under_two_percent_at_the_limit_is_not_flagged(tmp_path):
    tag = "qwen3.5-4b-iq4_xs"
    cell(tmp_path, tag, "tool_first", lookups=1000, ctoks=5)
    assert S.analyse(tmp_path, nb=200)[(tag, "tool_first")]["limit_flag"] is False
