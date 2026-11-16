import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ko import ablate, conditions as C, runner                          # noqa: E402
from ko.popqa import Item                                               # noqa: E402
from ko.store import Fact, FactStore                                    # noqa: E402
from test_harness import FACTS, GOLD, FakeClient, call, text           # noqa: E402

GOLD_FACT = Fact("George Rankin", "occupation", "politician")


def test_exclude_removes_only_the_own_triple():
    st = FactStore(FACTS + [Fact("George Rankin", "occupation", "soldier")])
    got = st.search("George Rankin occupation", k=5, exclude=GOLD_FACT)
    assert GOLD_FACT not in got and Fact("George Rankin", "occupation", "soldier") in got
    assert GOLD_FACT not in st.lookup("George Rankin", "occupation", exclude=GOLD_FACT)
    assert GOLD_FACT in st.search("George Rankin occupation", k=5)


def test_ablated_store_condition_never_shows_the_own_fact():
    client = FakeClient([text("I don't know")])
    ctx = C.Context(client=client, store=FactStore(FACTS), ablated=True, variant="abstain_ok")
    a = C.store(ctx, GOLD)
    user = client.calls[0]["messages"][1]["content"]
    assert "George Rankin | occupation | politician" not in user
    assert "George Rankin | place of birth | Sydney" in user and a.text == "I don't know"


def test_ablated_tool_lookup_finds_nothing_for_the_own_fact():
    client = FakeClient([call("lookup", entity="George Rankin", relation="occupation"), text("I don't know")])
    ctx = C.Context(client=client, store=FactStore([GOLD_FACT]), ablated=True, variant="abstain_ok")
    C.tool(ctx, GOLD)
    tool_msg = [m for m in client.calls[1]["messages"] if m["role"] == "tool"][0]["content"]
    assert tool_msg == "no stored facts match"


def test_abstain_ok_prompts_and_where_it_applies():
    assert [c for c in C.CONDITIONS if C.variant_for(c, "abstain_ok") == "abstain_ok"] == list(C.ABLATED_CONDITIONS)
    assert C.variant_for("oracle", "abstain_ok") == "default" and C.variant_for("web", "search_first") == "search_first"
    client = FakeClient([text("x")])
    C.closed(C.Context(client=client, variant="abstain_ok"), GOLD)
    assert "I don't know" in client.calls[0]["messages"][0]["content"]
    ctx = C.Context(variant="abstain_ok")
    s = C.system_for(ctx, "the lookup tool")
    assert "MUST first call the lookup tool" in s and "I don't know" in s


@pytest.mark.parametrize("pred,contains,want", [
    ("politician", True, "correct"),
    ("I don't know", False, "abstained"), ("I do not know.", False, "abstained"),
    ("The facts do not contain this.", False, "abstained"), ("I’m not sure", False, "abstained"),
    ("", False, "abstained"),
    ("journalist", False, "hallucinated"), ("Sydney", False, "hallucinated"),
    ("politician, but I'm not sure", True, "correct"),
])
def test_outcome_labels(pred, contains, want):
    assert ablate.outcome(pred, contains) == want


def test_run_cell_records_an_outcome_per_answer_and_a_summary():
    client = FakeClient([text("I don't know"), text("journalist"), text("politician")])
    items = [Item(str(i), "What is George Rankin's occupation?", "George Rankin", "occupation",
                  "politician", ("politician",), 10) for i in range(3)]
    ctx = C.Context(client=client, store=FactStore(FACTS), ablated=True, variant="abstain_ok")
    res = runner.run_cell(condition="closed", items=items, ctx=ctx, rep=0, meta={}, log=lambda *_: None)
    assert [r["outcome"] for r in res["answers"]] == ["abstained", "hallucinated", "correct"]
    assert res["summary"]["pass1"]["outcomes"]["hallucinated"] == pytest.approx(1 / 3)


def test_ablated_cells_have_their_own_names():
    assert runner.cell_path("m", 200, "closed", 0, arm="ablated", variant="abstain_ok") != \
        runner.cell_path("m", 200, "closed", 0, variant="abstain_ok")


def test_cmd_run_ablated_end_to_end(tmp_path, monkeypatch):
    import run
    tsv = tmp_path / "popqa.tsv"
    rows = ["id\tquestion\tsubj\tprop\tobj\tpossible_answers\ts_pop"]
    for i in range(30):
        rows.append(f"{i}\tWhat is S{i}'s occupation?\tS{i}\toccupation\tjob{i}\t[\"job{i}\"]\t{10 ** (i % 3 + 1)}")
    tsv.write_text("\n".join(rows) + "\n")

    class Fake:
        def __init__(self, *a, **k): pass
        def server_info(self): return {"endpoint": "e", "model_file": "Tiny.gguf", "quant": "Q8_0", "n_ctx": 1, "build": "b"}
        def chat(self, messages, **k): return text("I don't know")
    monkeypatch.setattr(run, "Client", Fake)
    monkeypatch.setattr(runner, "RESULTS", tmp_path / "results")
    monkeypatch.setattr(run.models, "verify", lambda f, **k: {"id": "t", "repo": "r", "file": f, "sha256": "0" * 64,
                                                               "size": 1, "quant": "Q8_0", "lineage": "l", "verified": "sha256"})
    argv = ["run.py", "run", "--model-tag", "t", "--data", str(tsv), "--n", "20", "--ablate-n", "5", "--ablated",
            "--prompt-variant", "abstain_ok", "--repeats", "1", "--sample", "stratified", "--dataset-seed", "1",
            "--exclude-ambiguous", "--allow-local", "--llm-url", "http://localhost:1", "--conditions", "closed", "store"]
    monkeypatch.setattr(sys, "argv", argv)
    run.main()
    files = sorted((tmp_path / "results").glob("*.json"))
    assert [f.name for f in files] == ["popqa-stratified1u-ablated-n5-t-closed-abstain_ok-r0.json",
                                       "popqa-stratified1u-ablated-n5-t-store-abstain_ok-r0.json"]
    r = json.loads(files[1].read_text())
    assert r["arm"] == "ablated" and r["exclude_ambiguous"] and r["model"]["id"] == "t"
    assert r["summary"]["pass1"]["outcomes"]["abstained"] == 1.0 and len(r["answers"]) == 5
    assert run.cell_label(r) == "store/abstain_ok ablated [stratified s1 unamb]"
    monkeypatch.setattr(sys, "argv", argv[:argv.index("--conditions")] + ["--conditions", "oracle"])
    with pytest.raises(SystemExit):
        run.main()


def test_label_script_picks_both_directions_and_scores_agreement():
    import label_abstain as L
    rows = [{"regex": "abstained", "human": "abstained", "id": str(i), "condition": "c", "prediction": "idk"} for i in range(30)] + \
           [{"regex": "hallucinated", "human": "hallucinated", "id": str(100 + i), "condition": "c", "prediction": "x"} for i in range(30)]
    got = L.pick(rows, 50, 0)
    assert len(got) == 50 and sum(r["regex"] == "abstained" for r in got) == 25
    assert len(L.pick(rows[:10], 50, 0)) == 10          # fewer than asked: take what exists
    rows[0]["human"] = "hallucinated"
    ag = L.agreement(rows)
    assert ag["agree"] == 59 and ag["disagreements"][0]["id"] == "0"
    rows[1]["human"] = "maybe"
    with pytest.raises(ValueError):
        L.agreement(rows)
