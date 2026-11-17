import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import verify_h5h8 as V   # noqa: E402


def cell(d, tag, cond, var, arm, n, errors=0, verified="sha256"):
    name = f"popqa-stratified1u{'-ablated' if arm == 'ablated' else ''}-n{n}-{tag}-{cond.replace('+', 'plus')}-{var}-r0.json"
    ans = [{"id": str(i), "pass": 1, "error": "x" if i < errors else ""} for i in range(n)]
    (d / name).write_text(json.dumps({"model_tag": tag, "condition": cond, "prompt_variant": var, "arm": arm, "n_questions": n,
                                      "answers": ans, "sample": "stratified", "dataset_seed": 1, "exclude_ambiguous": True,
                                      "model": {"verified": verified}}))


def full(d):
    for tag in V.TAGS:
        for arm, var, n in V.PLAN:
            for cond in V.CONDS:
                cell(d, tag, cond, var, arm, n)


def test_all_36_cells_present_and_clean(tmp_path):
    full(tmp_path)
    rows, bad = V.check(tmp_path)
    assert len(rows) == 36 and bad == 0 and all(r[4] == "ok" for r in rows)


def test_missing_short_errored_and_unverified_cells_are_flagged(tmp_path):
    full(tmp_path)
    (tmp_path / "popqa-stratified1u-n1000-qwen3.5-9b-iq4_xs-tool-default-r0.json").unlink()
    cell(tmp_path, "qwen3.5-2b-iq4_xs", "tool", "default", "main", 999)
    cell(tmp_path, "qwen3.5-4b-iq4_xs", "tool+contract", "default", "main", 1000, errors=2)
    cell(tmp_path, "qwen3.5-0.8b-q8_0", "tool", "abstain_default", "ablated", 200, verified="name-only")
    rows, bad = V.check(tmp_path)
    st = {(r[0], r[1], r[2]): r for r in rows}
    assert st[("qwen3.5-9b-iq4_xs", "main", "tool")][4] == "MISSING"
    assert "999 answers" in st[("qwen3.5-2b-iq4_xs", "main", "tool")][5]
    assert "2 answers with an error" in st[("qwen3.5-4b-iq4_xs", "main", "tool+contract")][5]
    assert "not sha256-verified" in st[("qwen3.5-0.8b-q8_0", "ablated", "tool")][5]
    assert bad == 4


def test_the_search_first_cells_of_E1_do_not_count(tmp_path):
    cell(tmp_path, "qwen3.5-9b-iq4_xs", "tool", "search_first", "main", 1000)
    rows, _ = V.check(tmp_path)
    assert next(r for r in rows if r[:3] == ("qwen3.5-9b-iq4_xs", "main", "tool"))[4] == "MISSING"
