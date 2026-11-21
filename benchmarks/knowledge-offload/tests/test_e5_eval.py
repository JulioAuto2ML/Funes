"""E5 eval plan, set P loading, the transfer rule in the adapter gate, and the verifier on synthetic cells."""
import dataclasses
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

import pytest

import e5_eval as E
import verify_e5 as V
from ko import models, popqa


def item(i, subj="Paris", q="What is the capital of France?"):
    return popqa.Item(id=str(i), question=q.replace("Paris", subj), subj=subj, prop="capital", obj="x", aliases=("x", "y"), s_pop=10)


def prow(it, q):
    return {"id": it.id, "original": it.question, "question": q, "template": 0, "prop": it.prop, "aliases": list(it.aliases)}


def write(tmp_path, rows):
    p = tmp_path / "P.jsonl"
    p.write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    return p


def test_plan_has_every_arm_and_unique_cells():
    cs = E.cells()
    assert len(cs) == len({V.cell_key(c) for c in cs}) == 21
    assert {c.tag for c in cs} == {f"qwen3.5-{s}-iq4_xs-{a}" for s in ("2b", "0.8b") for a in ("l1", "l0")} | {"qwen3.5-2b-q8_0-l1"}
    assert sum(c.task == "gsm8k" for c in cs) == 4 and all(c.n == 500 for c in cs if c.task == "gsm8k")
    assert {c.n for c in cs if c.paraphrase} == {200} and {c.n for c in cs if c.arm == "ablated"} == {200}


def test_every_planned_adapter_is_in_the_lock_and_its_base_is_the_plan_s():
    lock = {a["id"]: a for a in models.load_adapters()}
    for c in E.cells():
        a = lock[c.adapter]
        assert a["base"] == c.base or c.base in a.get("transfer_bases", [])


def test_commands_name_the_adapter_and_use_distinct_tags():
    argv = E.Cell("t-l1", "b", "l1-b-2b", "popqa", "tool", variant="abstain_default", arm="ablated").argv()
    assert argv[argv.index("--expect-adapter") + 1] == "l1-b-2b" and "--ablated" in argv and "abstain_default" in argv
    assert "--paraphrase" in E.Cell("t", "b", "a", "popqa", "tool", paraphrase=True).argv()


def test_paraphrased_replaces_only_the_question(tmp_path):
    items = [item(1), item(2, "Rome", "What is the capital of Rome?")]
    p = write(tmp_path, [prow(items[0], "Paris is the capital of which country?"), prow(items[1], "Name the capital of Rome.")])
    got, meta = E.paraphrased(items, p)
    assert [g.id for g in got] == ["1", "2"] and got[0].question == "Paris is the capital of which country?"
    assert got[0].aliases == items[0].aliases and meta["n"] == 2 and len(meta["sha256"]) == 64


def test_paraphrased_may_be_shorter_than_the_sample(tmp_path):
    items = [item(1), item(2)]
    got, _ = E.paraphrased(items, write(tmp_path, [prow(items[0], "Name Paris's role.")]))
    assert len(got) == 1


@pytest.mark.parametrize("bad", ["other_id", "same_question", "lost_subject"])
def test_paraphrased_refuses_a_file_that_does_not_fit(tmp_path, bad):
    it = item(1)
    row = prow(it, "Which city is the capital?" if bad != "same_question" else it.question)
    if bad == "other_id":
        row["id"] = "9"
    if bad == "lost_subject":
        row["question"] = "Which city is it?"
    with pytest.raises(SystemExit):
        E.paraphrased([it], write(tmp_path, [row]))


def test_adapter_served_on_a_transfer_base_is_accepted_and_flagged(tmp_path):
    f = tmp_path / "x.gguf"
    f.write_bytes(b"abc")
    sha = models.file_sha256(f, None)
    ad = [{"id": "a", "file": "x.gguf", "sha256": sha, "base": "b1", "transfer_bases": ["b2"]}]
    act = [{"path": "/m/x.gguf", "scale": 1.0}]
    out = models.verify_adapters(act, base_id="b2", adapters=ad, adapters_dir=tmp_path, expect="a", cache_path=None)
    assert out[0]["transfer"] is True
    assert models.verify_adapters(act, base_id="b1", adapters=ad, adapters_dir=tmp_path, expect="a", cache_path=None)[0]["transfer"] is False
    with pytest.raises(models.ModelRefused):
        models.verify_adapters(act, base_id="b3", adapters=ad, adapters_dir=tmp_path, expect="a", cache_path=None)


# ---- verifier ----
def result(c: E.Cell, n=None, **over):
    n = c.n if n is None else n
    lock = {a["id"]: a for a in models.load_adapters()}[c.adapter]
    r = {"model_tag": c.tag, "condition": c.condition, "prompt_variant": c.variant, "arm": c.arm, "n_questions": n,
         "sample": "stratified", "dataset_seed": 1, "exclude_ambiguous": True,
         "model": {"id": c.base, "verified": "sha256"},
         "adapters": [{**lock, "scale": 1.0, "verified": "sha256", "transfer": lock["base"] != c.base}],
         "paraphrase": {"file": "P.jsonl", "sha256": "ab" * 32, "n": 200} if c.paraphrase else None,
         "answers": [{"id": str(i), "pass": 1, "error": None} for i in range(n)]}
    if c.task == "gsm8k":
        r.update(dataset="gsm8k-test", dataset_seed=1, pilot=False)
        for k in ("sample", "exclude_ambiguous", "arm", "prompt_variant"):
            r.pop(k)
        r["answers"] = [{"id": str(i), "error": None} for i in range(n)]      # GSM8K rows carry no `pass`
    r.update(over)
    return r


def put(tmp_path, c, **over):
    r = result(c, **over)
    (tmp_path / f"{'gsm8k' if c.task == 'gsm8k' else 'popqa'}-{abs(hash(V.cell_key(c)))}.json").write_text(json.dumps(r))


def full(tmp_path, skip=(), **over):
    for i, c in enumerate(E.cells()):
        if i not in skip:
            put(tmp_path, c)


def test_verifier_passes_a_complete_clean_set(tmp_path):
    full(tmp_path)
    rows, bad = V.check(tmp_path)
    assert bad == 0 and all(r[1] == "ok" for r in rows), rows


def test_verifier_flags_missing_cell(tmp_path):
    full(tmp_path, skip=(3,))
    rows, bad = V.check(tmp_path)
    assert bad == 1 and sum(r[1] == "MISSING" for r in rows) == 1


@pytest.mark.parametrize("over,needle", [
    ({"adapters": []}, "adapters"),
    ({"model": {"id": "qwen3.5-9b-iq4_xs", "verified": "sha256"}}, "served"),
    ({"model": {"id": None, "verified": "name"}}, "sha256-verified"),
    ({"sample": "random"}, "E1 sample"),
])
def test_verifier_flags_a_wrong_cell(tmp_path, over, needle):
    c = E.cells()[0]
    full(tmp_path, skip=(0,))
    put(tmp_path, c, **over)
    rows, bad = V.check(tmp_path)
    assert bad >= 1 and needle in [r for r in rows if r[1] == "BAD"][0][2]


def test_verifier_flags_errors_short_and_duplicate(tmp_path):
    cs = E.cells()
    full(tmp_path, skip=(0, 1))
    put(tmp_path, cs[0], answers=[{"id": str(i), "pass": 1, "error": "x" if i == 0 else None} for i in range(1000)])
    put(tmp_path, cs[1], n=150)
    rows, bad = V.check(tmp_path)
    assert bad == 2 and "error" in rows[0][2] and "answers" in rows[1][2]
    (tmp_path / "popqa-dup.json").write_text(json.dumps(result(cs[0])))
    rows, bad = V.check(tmp_path)
    assert "2 files" in rows[0][2]


def test_verifier_requires_the_first_200_ids_for_p(tmp_path):
    cs = E.cells()
    full(tmp_path)
    pf = tmp_path / "P.jsonl"
    pf.write_text("\n".join(json.dumps({"id": str(i)}) for i in range(1, 201)) + "\n")      # shifted by one
    rows, bad = V.check(tmp_path, pf)
    assert any("first 200" in r[2] for r in rows)
    pf.write_text("\n".join(json.dumps({"id": str(i)}) for i in range(200)) + "\n")
    assert V.check(tmp_path, pf)[1] == 0


def test_verifier_flags_cells_run_on_different_p_files(tmp_path):
    cs = E.cells()
    full(tmp_path)
    c = next(c for c in cs if c.paraphrase)
    for f in tmp_path.glob("popqa-*.json"):
        r = json.loads(f.read_text())
        if r["model_tag"] == c.tag and r.get("paraphrase"):
            r["paraphrase"]["sha256"] = "cd" * 32
            f.write_text(json.dumps(r))
    rows, bad = V.check(tmp_path)
    assert any(r[0] == "set P" for r in rows)
