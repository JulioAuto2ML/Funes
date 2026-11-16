import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import stats_e0 as S                                                    # noqa: E402


def write_data(tmp):
    rows = ["id\tquestion\tsubj\tprop\tobj\tpossible_answers\ts_pop",
            "1\tq1\tThe Walk\tgenre\tfilm\t[\"film\"]\t10", "2\tq2\tThe Walk\tgenre\talbum\t[\"album\"]\t10"]
    rows += [f"{i}\tq{i}\tS{i}\toccupation\tjob{i}\t[\"job{i}\"]\t{10 ** (i % 3 + 1)}" for i in range(3, 13)]
    p = tmp / "d.tsv"
    p.write_text("\n".join(rows) + "\n")
    return p


def cell(tmp, name, condition, correct, *, tag="m", rep=0, variant="default", arm="main", passes=(1,), outcome=None):
    ans = []
    for p in passes:
        for i in range(1, 13):
            a = {"id": str(i), "pass": p, "pop_bucket": f"1e{1 + i % 3}", "contains": i in correct,
                 "prediction": "x", "hedge": False}
            if outcome:
                a["outcome"] = outcome(i)
            ans.append(a)
    (tmp / name).write_text(json.dumps({"model_tag": tag, "condition": condition, "repeat": rep,
                                        "prompt_variant": variant, "arm": arm, "dataset_seed": 0,
                                        "sample": "random", "answers": ans}))


def test_cells_are_found_by_meta_and_ablated_or_other_tags_stay_out(tmp_path):
    cell(tmp_path, "popqa-n12-m-closed-r0.json", "closed", {3})
    cell(tmp_path, "popqa-n12-m-closed-r1.json", "closed", {3}, rep=1)
    cell(tmp_path, "popqa-n12-other-closed-r0.json", "closed", {3, 4}, tag="other")
    cell(tmp_path, "popqa-ablated-n12-m-closed-abstain_ok-r0.json", "closed", {3}, variant="abstain_ok", arm="ablated")
    cells = S.find_cells(tmp_path, "popqa", "m")
    assert list(cells) == ["closed"] and len(cells["closed"]) == 2


def test_the_table_is_restricted_to_unambiguous_items_and_averages_repeats(tmp_path, capsys):
    data = write_data(tmp_path)
    cell(tmp_path, "popqa-n12-m-closed-r0.json", "closed", {1, 3, 4})
    cell(tmp_path, "popqa-n12-m-closed-r1.json", "closed", {1, 3}, rep=1)
    cell(tmp_path, "popqa-n12-m-store-r0.json", "store", set(range(1, 13)))
    S.main(["--tag", "m", "--results", str(tmp_path), "--data", str(data), "--out", str(tmp_path / "t.md")])
    out = (tmp_path / "t.md").read_text()
    assert "Questions: 10" in out                                           # 12 minus the two "The Walk" rows
    assert "| closed | 2 | 0.150 |" in out and "| store | 1 | 1.000 |" in out
    assert "| store | closed | +0.850 |" in out
    S.main(["--tag", "m", "--results", str(tmp_path), "--data", str(data), "--all-items", "--out", str(tmp_path / "a.md")])
    assert "Questions: 12" in (tmp_path / "a.md").read_text()


def test_two_pass_conditions_and_the_ablated_section(tmp_path):
    data = write_data(tmp_path)
    cell(tmp_path, "popqa-n12-m-closed-r0.json", "closed", {3})
    cell(tmp_path, "popqa-n12-m-webpluscache-r0.json", "web+cache", {3, 4}, passes=(1, 2))
    cell(tmp_path, "popqa-ablated-n12-m-closed-abstain_ok-r0.json", "closed", {3}, variant="abstain_ok", arm="ablated",
         outcome=lambda i: "correct" if i == 3 else "hallucinated")
    S.main(["--tag", "m", "--results", str(tmp_path), "--data", str(data), "--out", str(tmp_path / "t.md")])
    out = (tmp_path / "t.md").read_text()
    assert "| web+cache p1 |" in out and "| web+cache p2 |" in out
    assert "## Ablated arm" in out and "closed/abstain_ok" in out
