"""E5 data builder: disjointness, determinism, the held-out rule, round-trip, masks, anchors, P scaffolding.
Synthetic fixture TSV (constants shrunk with monkeypatch); the real PopQA file is used only if it is on
disk. No network, no model."""
import csv
import hashlib
import json
import random
import sys
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

import pytest

import e5_build
from ko import e5_data as D, e5_paraphrase as P, popqa
from ko.popqa import Item
from ko.store import Fact, FactStore

PROPS = ["capital", "capital of", "director", "author", "genre", "sport", "occupation", "color"]
COLS = ["id", "subj", "prop", "obj", "s_uri", "s_pop", "question", "possible_answers"]


def write_tsv(path: Path, n_subj=260, drop_uri=False) -> Path:
    """Each subject has two triples (two different props) under one s_uri; (subj, prop) is unique."""
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, COLS + ["s_aliases"], delimiter="\t")
        w.writeheader()
        k = 0
        for s in range(n_subj):
            for j in range(2):
                prop = PROPS[(s + j * 3) % len(PROPS)]
                k += 1
                w.writerow({"id": str(k), "subj": f"Subj {s}", "prop": prop, "obj": f"Obj {k}",
                            "s_uri": "" if drop_uri else f"http://x/Q{s}", "s_pop": 10 ** (s % 5 + 1),
                            "question": f"What is Subj {s}'s {prop}?",
                            "possible_answers": json.dumps([f"Obj {k}", f"Alias {k}"]), "s_aliases": "[]"})
    return path


@pytest.fixture
def small(monkeypatch):
    """Constants shrunk to fixture size: 80-item eval, held-out units need >= 6 eval items."""
    monkeypatch.setattr(D, "EVAL_N", 80)
    monkeypatch.setattr(D, "HELDOUT_MIN_EVAL", 6)
    monkeypatch.setattr(D, "HELDOUT_MIN_TOTAL", 30)
    monkeypatch.setattr(D, "DEV_N", 30)


@pytest.fixture
def world(small, tmp_path):
    path = write_tsv(tmp_path / "popqa.tsv")
    items = popqa.load(path)
    return path, items, D.load_s_uris(path), D.eval_items(items)


def split_of(world):
    path, items, uri, ev = world
    held = D.draw_heldout(ev)
    return D.build_split(items, ev, held, uri), held, uri, items


# ---- eval sample ----
def test_eval_sample_is_the_harness_stratified_unambiguous_sample(world):
    _, items, _, ev = world
    assert ev == popqa.stratified_sample(popqa.unambiguous(items), D.EVAL_N, D.EVAL_SEED) and len(ev) == 80


def test_check_against_cell(world, tmp_path):
    ev = world[3]
    cell = tmp_path / "cell.json"
    assert D.check_against_cell(ev, cell) is None
    cell.write_text(json.dumps({"answers": [{"id": i.id} for i in ev]}))
    assert D.check_against_cell(ev, cell) is True
    cell.write_text(json.dumps({"answers": [{"id": i.id} for i in ev[1:]]}))
    assert D.check_against_cell(ev, cell) is False


def test_absent_cell_is_loud_and_recorded_in_the_manifest(world, tmp_path):
    logs = []
    m = D.build(world[0], tmp_path / "o", n_lookup=20, log=logs.append, cell_path=tmp_path / "nope.json")
    assert m["eval_cell_check"] == "ABSENT" and any("WARNING" in l and "ABSENT" in l for l in logs)
    cell = tmp_path / "cell.json"
    cell.write_text(json.dumps({"answers": [{"id": i.id} for i in world[3]]}))
    logs = []
    assert D.build(world[0], tmp_path / "p", n_lookup=20, log=logs.append, cell_path=cell)["eval_cell_check"] == "MATCH"
    assert not any("WARNING" in l and "cell" in l for l in logs)
    cell.write_text(json.dumps({"answers": []}))
    logs = []
    assert D.build(world[0], tmp_path / "q", n_lookup=20, log=logs.append, cell_path=cell)["eval_cell_check"] == "MISMATCH"
    assert any("WARNING" in l and "MISMATCH" in l for l in logs)


def test_missing_s_uri_is_refused(small, tmp_path):
    with pytest.raises(D.E5DataError, match="s_uri"):
        D.load_s_uris(write_tsv(tmp_path / "x.tsv", n_subj=3, drop_uri=True))


# ---- held-out rule ----
def test_capital_and_capital_of_are_one_unit_and_color_is_out(world):
    ev = world[3]
    counts = D.unit_counts(ev)
    assert "capital of" not in counts and counts["capital"] == sum(i.prop in ("capital", "capital of") for i in ev)
    held = D.draw_heldout(ev)
    assert "color" not in held.units and len(held.units) == 3


def test_draw_is_random_5_sample_over_sorted_eligible_units(world, monkeypatch):
    ev = world[3]
    monkeypatch.setattr(D, "HELDOUT_MIN_TOTAL", 0)
    counts = D.unit_counts(ev)
    eligible = sorted(u for u, c in counts.items() if c >= D.HELDOUT_MIN_EVAL and u != "color")
    held = D.draw_heldout(ev)
    assert held.units == tuple(sorted(random.Random(5).sample(eligible, 3)))
    assert held.seed == 5 and len(held.attempts) == 1 and not held.below_threshold


def test_units_under_the_minimum_are_never_drawn(world, monkeypatch):
    ev = world[3]
    monkeypatch.setattr(D, "HELDOUT_MIN_EVAL", 10 ** 6)
    with pytest.raises(D.E5DataError, match="only 0 units"):
        D.draw_heldout(ev)


def test_redraw_branch_taken_logged_and_second_draw_kept(world, monkeypatch):
    ev = world[3]
    totals = {}
    for s in range(1, 60):
        monkeypatch.setattr(D, "HELDOUT_SEEDS", (s, s + 100))
        monkeypatch.setattr(D, "HELDOUT_MIN_TOTAL", 0)
        totals[s] = D.draw_heldout(ev).eval_total
    first, second = next((a, b) for a in totals for b in totals if totals[a] < totals[b])
    monkeypatch.setattr(D, "HELDOUT_SEEDS", (first, second))
    monkeypatch.setattr(D, "HELDOUT_MIN_TOTAL", totals[second])
    held = D.draw_heldout(ev)
    assert [a["seed"] for a in held.attempts] == [first, second] and held.seed == second
    assert held.eval_total == totals[second] and not held.below_threshold
    assert "REDRAW" in D.describe_draw(held, ev)


def test_redraw_still_short_is_flagged_not_repeated(world, monkeypatch):
    ev = world[3]
    monkeypatch.setattr(D, "HELDOUT_MIN_TOTAL", 10 ** 6)
    held = D.draw_heldout(ev)
    assert len(held.attempts) == 2 and held.below_threshold and held.seed == D.HELDOUT_SEEDS[1]
    assert "WARNING" in D.describe_draw(held, ev)


def test_real_threshold_constants_match_the_design():
    assert (D.HELDOUT_SEEDS, D.HELDOUT_N, D.HELDOUT_MIN_EVAL, D.HELDOUT_MIN_TOTAL) == ((5, 6), 3, 40, 150)
    assert (D.EVAL_N, D.EVAL_SEED, D.DEV_N) == (1000, 1, 300)


# ---- disjointness ----
def test_split_is_disjoint_from_eval_by_id_subject_uri_and_heldout_relation(world):
    split, held, uri, items = split_of(world)
    ev_ids = {i.id for i in split.eval}
    ev_uris = {uri[i] for i in ev_ids}
    assert split.pool and split.dev
    for part in (split.pool, split.dev):
        assert not ev_ids & {i.id for i in part}
        assert not ev_uris & {uri[i.id] for i in part}
        assert not {D.unit_of(i.prop) for i in part} & set(held.units)
    assert not {uri[i.id] for i in split.dev} & {uri[i.id] for i in split.pool}
    assert len(split.dev) == D.DEV_N
    sharing = [i for i in items if uri[i.id] in ev_uris and i.id not in ev_ids]
    assert sharing and not {i.id for i in sharing} & {i.id for i in split.pool}   # the case the rule exists for


def test_assert_disjoint_raises_for_each_violation(world):
    split, held, uri, items = split_of(world)
    by_uri = {}
    for i in items:
        by_uri.setdefault(uri[i.id], []).append(i)
    sibling = next(i for i in by_uri[uri[split.eval[0].id]] if i.id != split.eval[0].id)
    held_item = next(i for i in items if D.unit_of(i.prop) in held.units)
    other = next(i for i in split.dev)
    bad = {
        "eval id": D.Split(split.eval, split.dev, (*split.pool, split.eval[0]), split.held_out),
        "subject URI": D.Split(split.eval, split.dev, (*split.pool, sibling), split.held_out),
        "held-out relation": D.Split(split.eval, split.dev, (*split.pool, held_item), split.held_out),
        "dev and train": D.Split(split.eval, split.dev, (*split.pool, other), split.held_out),
    }
    for name, s in bad.items():
        with pytest.raises(D.E5DataError):
            D.assert_disjoint(s, uri)
    D.assert_disjoint(split, uri)


def test_ambiguous_triples_are_not_trained_on(small, tmp_path):
    path = write_tsv(tmp_path / "a.tsv")
    rows = list(csv.DictReader(open(path, encoding="utf-8"), delimiter="\t"))
    rows.append({**rows[0], "id": "9999", "obj": "Other", "possible_answers": '["Other"]'})   # homonym: 2 objects
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, list(rows[0]), delimiter="\t")
        w.writeheader()
        w.writerows(rows)
    items, uri = popqa.load(path), D.load_s_uris(path)
    ev = D.eval_items(items)
    split = D.build_split(items, ev, D.draw_heldout(ev), uri)
    assert rows[0]["id"] not in {i.id for i in split.pool} and "9999" not in {i.id for i in split.pool}


# ---- buckets ----
def test_bucket_targets_follow_eval_proportions(world):
    ev = world[3]
    t = D.bucket_targets(len(ev), ev)
    assert t == dict(sorted(Counter(popqa.pop_bucket(i) for i in ev).items()))
    t2 = D.bucket_targets(33, ev)
    assert sum(t2.values()) == 33 and set(t2) == set(t)


def test_short_bucket_is_capped_and_shortfall_redistributed_by_largest_remainder(world):
    split = split_of(world)[0]
    have = Counter(popqa.pop_bucket(i) for i in split.pool)
    b_small = min(have, key=have.get)
    others = [b for b in have if b != b_small]
    targets = {b_small: have[b_small] + 10, **{b: 3 for b in others}}
    n = sum(targets.values())
    chosen, report = D.sample_by_bucket(split.pool, targets, 1)
    assert len(chosen) == n and len({i.id for i in chosen}) == n            # n kept, no duplicates anywhere
    r = report[b_small]
    assert r["taken"] == have[b_small] and r["extra"] == 0 and r["short"] == 10
    assert sum(v["extra"] for v in report.values()) == 10 and sum(v["taken"] for v in report.values()) == n
    for b in others:
        assert report[b]["taken"] == 3 + report[b]["extra"] <= have[b]
    got = Counter(popqa.pop_bucket(i) for i in chosen)
    assert all(got[b] == report[b]["taken"] for b in report)
    text = D.describe_buckets(report, D.pool_bucket_counts(split.pool, split.eval))
    assert "OVER-REPRESENTED" in text and "extra" in text


def test_redistribution_respects_spare_pool_and_empty_buckets_stay_empty(world):
    split = split_of(world)[0]
    have = Counter(popqa.pop_bucket(i) for i in split.pool)
    chosen, report = D.sample_by_bucket(split.pool, {"1e9": 7, **{b: 2 for b in have}}, 1)
    assert report["1e9"] == {"target": 7, "pool": 0, "taken": 0, "extra": 0, "short": 7}
    assert all(popqa.pop_bucket(i) != "1e9" for i in chosen) and len(chosen) == 2 * len(have) + 7
    assert all(report[b]["taken"] <= have[b] for b in have)
    # more asked than the whole pool holds: everything is taken once, the rest is reported short
    chosen, report = D.sample_by_bucket(split.pool, {b: 10 ** 5 for b in have}, 1)
    assert len(chosen) == len(split.pool) == len({i.id for i in chosen})
    assert all(r["short"] == r["target"] - r["taken"] > 0 for r in report.values())
    assert "SHORT" in D.describe_buckets(report, D.pool_bucket_counts(split.pool, split.eval))


def test_sampling_is_deterministic_and_seed_sensitive(world):
    split = split_of(world)[0]
    t = D.bucket_targets(40, split.eval)
    a = D.sample_by_bucket(split.pool, t, 3)[0]
    assert a == D.sample_by_bucket(split.pool, t, 3)[0] and a != D.sample_by_bucket(split.pool, t, 4)[0]


def test_dev_is_drawn_proportionally_from_candidates_and_counts_are_logged_first(world):
    path, items, uri, ev = world
    logs = []
    held = D.draw_heldout(ev)
    split = D.build_split(items, ev, held, uri, log=logs.append)
    cands = [i for i in popqa.unambiguous(items) if i.id not in {e.id for e in ev}
             and uri[i.id] not in {uri[e.id] for e in ev} and D.unit_of(i.prop) not in held.units]
    assert len(split.dev) == D.DEV_N and len(logs) == 1 and logs[0].startswith("dev candidates per bucket")
    counts = Counter(popqa.pop_bucket(i) for i in cands)
    assert split.cand_bucket_counts == dict(sorted(counts.items()))
    assert all(f"{b}={c}" in logs[0] for b, c in counts.items())
    assert split.dev == tuple(random.Random(D.DEV_SEED).sample(cands, D.DEV_N))      # not stratified


# ---- examples ----
@pytest.fixture
def pair(world):
    path, items, uri, ev = world
    split = split_of(world)[0]
    chosen, _ = D.sample_by_bucket(split.pool, D.bucket_targets(40, ev), 3)
    store = D.runner_store(items)
    l1, l0, dropped = D.build_pair(chosen, store)
    return chosen, l1, l0, dropped, store


def test_l1_example_shape_and_answer_is_first_alias(pair):
    chosen, l1, _, dropped, store = pair
    item = chosen[0]
    row = next(r for r in l1 if r["id"] == f"L1:lookup:{item.id}")
    sysm, user, call, tool, final = row["messages"]
    assert sysm["content"] == D.C.SYSTEM_TOOL and user["content"] == item.question
    assert json.loads(call["tool_calls"][0]["function"]["arguments"]) == {"entity": item.subj, "relation": item.prop}
    assert tool["content"] == D.T.run_lookup(store, {"entity": item.subj, "relation": item.prop})
    assert final["content"] == item.aliases[0] and row["tools"] == [D.T.LOOKUP]
    assert final["content"] == item.aliases[0] and row["tools"] == [D.T.LOOKUP]


def test_l0_differs_from_l1_only_by_the_call_and_the_result(pair):
    _, l1, l0, _, _ = pair
    assert len(l1) == len(l0) > 0
    for a, b in zip(l1, l0):
        assert a["id"].split(":")[-1] == b["id"].split(":")[-1]
        # same system prompt, same user question, same final answer, same tools block, same template kwargs
        assert a["messages"][0] == b["messages"][0] == {"role": "system", "content": D.C.SYSTEM_TOOL}
        assert a["messages"][1] == b["messages"][1] and a["messages"][-1] == b["messages"][-1]
        assert b["tools"] == a["tools"] == [D.T.LOOKUP] and a["chat_template_kwargs"] == b["chat_template_kwargs"]
        assert len(b["messages"]) == 3 and a["messages"][2:4] != b["messages"][2:3]
        assert not any(m.get("tool_calls") or m["role"] == "tool" for m in b["messages"])
        # removing the call and result from L1 gives exactly L0's messages
        assert [m for k, m in enumerate(a["messages"]) if k not in (2, 3)] == b["messages"]


def test_mask_is_assistant_tokens_only_and_results_are_masked(pair):
    _, l1, l0, _, _ = pair
    row = l1[0]
    assert [(m["role"], m["what"], m["train"]) for m in row["mask"]] == [
        ("system", "prompt", False), ("user", "prompt", False), ("assistant", "call", True),
        ("tool", "tool result", False), ("assistant", "answer", True)]
    assert [(m["what"], m["train"]) for m in l0[0]["mask"]] == [("prompt", False), ("prompt", False), ("answer", True)]
    assert all(len(r["mask"]) == len(r["messages"]) for r in l1 + l0)
    assert all(m["train"] == (m["role"] == "assistant") for r in l1 + l0 for m in r["mask"])


def test_rendered_call_round_trips_through_llm_py(pair):
    _, l1, _, _, _ = pair
    for row in l1:
        call = row["messages"][2]
        parsed = D.parse_with_harness(call)
        want = json.loads(call["tool_calls"][0]["function"]["arguments"])
        assert [(c["name"], c["arguments"]) for c in parsed] == [("lookup", want)]
        D.verify_roundtrip(row)


def test_roundtrip_failure_is_an_error(pair):
    row = json.loads(json.dumps(pair[1][0]))
    row["messages"][2]["tool_calls"][0]["function"]["arguments"] = "{not json"
    with pytest.raises(D.E5DataError, match="round-trip"):
        D.verify_roundtrip(row)
    D.verify_roundtrip(pair[2][0])          # an L0 row has no call: nothing to parse


def test_template_messages_turn_arguments_into_dicts_without_touching_the_row(pair):
    row = pair[1][0]
    before = json.dumps(row)
    msgs = D.template_messages(row)
    assert isinstance(msgs[2]["tool_calls"][0]["function"]["arguments"], dict) and json.dumps(row) == before


def test_an_example_whose_result_lacks_the_answer_is_dropped_and_listed(world):
    items = world[1]
    store = FactStore([Fact(i.subj, i.prop, "something else") for i in items[:5]])
    l1, l0, dropped = D.build_pair(items[:5], store)
    assert l1 == [] and l0 == [] and dropped == [i.id for i in items[:5]]


def test_system_and_tool_block_are_the_evals(pair):
    row = pair[1][0]
    assert row["messages"][0]["content"] == D.C.SYSTEM_TOOL and row["tools"][0] is not D.T.LOOKUP
    assert row["chat_template_kwargs"] == {"enable_thinking": False}


# ---- build: draw first, bytes, determinism ----
def test_build_prints_the_draw_before_anything_else_and_is_byte_deterministic(world, tmp_path):
    path = world[0]
    logs = []
    m1 = D.build(path, tmp_path / "a", n_lookup=40, log=logs.append)
    assert logs[0].startswith("eval items per relation") and "held out:" in logs[0]
    assert logs[1].startswith("WARNING") and "MISMATCH" in logs[1]      # fixture is not the real 1000-item cell
    assert logs[2].startswith("dev candidates per bucket")
    assert logs[3].startswith("popularity buckets")
    assert any(l.startswith("dropped_ungrounded: ") for l in logs)
    m2 = D.build(path, tmp_path / "b", n_lookup=40, log=lambda *_: None)
    names = sorted(p.name for p in (tmp_path / "a").iterdir())
    assert names == sorted(p.name for p in (tmp_path / "b").iterdir()) and "heldout_draw.json" in names
    for n in names:
        a, b = (tmp_path / "a" / n).read_bytes(), (tmp_path / "b" / n).read_bytes()
        assert a == b, n
        print(f"sha256 {hashlib.sha256(a).hexdigest()}  {n}")
    assert m1 == m2 and m1["n_lookup_written"] + len(m1["dropped_ungrounded"]) == 40
    assert json.loads((tmp_path / "a" / "heldout_draw.json").read_text())["units"] == list(m1["heldout"]["units"])


def test_build_output_contains_no_eval_item(world, tmp_path):
    path, items, uri, ev = world
    D.build(path, tmp_path / "o", n_lookup=40, log=lambda *_: None)
    ev_ids = {i.id for i in ev}
    ev_uris = {uri[i] for i in ev_ids}
    rows = [json.loads(l) for f in ("lookup_L1_n40.jsonl", "lookup_L0_n40.jsonl", "dev_items.jsonl")
            for l in (tmp_path / "o" / f).read_text().splitlines()]
    ids = {r["id"].split(":")[-1] for r in rows}
    assert rows and not ids & ev_ids and not {uri[i] for i in ids} & ev_uris
    assert json.loads((tmp_path / "o" / "eval_ids.json").read_text()) == [i.id for i in ev]


# ---- anchors ----
def anchor_files(tmp_path, n=12, empty=(), test_overlap=None, finish=None, drop_finish=()):
    prompts = tmp_path / "train.jsonl"
    prompts.write_text("".join(json.dumps({"question": f"Q{i}: how many?", "answer": "x #### 1"}) + "\n"
                               for i in range(n)))
    answers = tmp_path / "base.jsonl"
    rows = []
    for i in range(n):
        r = {"idx": i, "text": "" if i in empty else f"base says {i}", "finish_reason": (finish or {}).get(i, "stop")}
        if i in drop_finish:
            del r["finish_reason"]
        rows.append(r)
    answers.write_text("".join(json.dumps(r) + "\n" for r in rows))
    test = tmp_path / "test.jsonl"
    test.write_text(json.dumps({"question": test_overlap or "unrelated"}) + "\n")
    return prompts, answers, test


def test_anchor_rows_keep_base_answers_verbatim_half_with_tools_by_parity_in_both_arms(tmp_path):
    p, a, t = anchor_files(tmp_path)
    stats = {}
    rows = D.build_anchor_rows(p, a, 12, test_path=t, stats=stats)
    assert set(rows) == {"L1", "L0"} and len(rows["L1"]) == len(rows["L0"]) == 12
    for r1, r0 in zip(rows["L1"], rows["L0"]):
        assert r1["messages"] == r0["messages"] and r1["messages"][0]["content"] == D.ANCHOR_SYSTEM
        assert D.ANCHOR_SYSTEM == D.gsm8k.SYSTEM
        i = int(r1["id"].split(":")[-1])
        assert r1["messages"][-1]["content"] == f"base says {i}" and r1["kind"] == "anchor"
        want = [D.T.LOOKUP] if i % 2 == 0 else None                # even line id: with the tools block
        assert r1["tools"] == r0["tools"] == want
        assert [m["train"] for m in r1["mask"]] == [False, False, True]
    assert stats["with_tools"] == 6 and stats["without_tools"] == 6
    assert rows == D.build_anchor_rows(p, a, 12, test_path=t)


def test_anchor_empty_answers_are_skipped_and_shortage_is_an_error(tmp_path):
    p, a, _ = anchor_files(tmp_path, n=6, empty=(0, 1, 2))
    assert len(D.build_anchor_rows(p, a, 3)["L1"]) == 3
    with pytest.raises(D.E5DataError, match="not padded"):
        D.build_anchor_rows(p, a, 4)


def test_anchor_rows_cut_off_at_max_tokens_are_dropped_and_counted(tmp_path):
    p, a, _ = anchor_files(tmp_path, n=8, finish={0: "length", 5: "length"}, empty=(1,))
    stats = {}
    rows = D.build_anchor_rows(p, a, 5, stats=stats)
    assert stats["dropped_length"] == 2 and stats["skipped_empty"] == 1 and stats["usable"] == 5
    assert not {0, 1, 5} & {int(r["id"].split(":")[-1]) for r in rows["L1"]}
    with pytest.raises(D.E5DataError, match="not padded"):
        D.build_anchor_rows(p, a, 6)


def test_anchor_row_without_finish_reason_is_an_error(tmp_path):
    p, a, _ = anchor_files(tmp_path, n=4, drop_finish=(2,))
    with pytest.raises(D.E5DataError, match="finish_reason"):
        D.build_anchor_rows(p, a, 1)


def test_anchor_prompt_that_is_a_test_question_is_refused(tmp_path):
    p, a, t = anchor_files(tmp_path, test_overlap="q3: HOW many")
    with pytest.raises(D.E5DataError, match="test questions"):
        D.build_anchor_rows(p, a, 12, test_path=t)


def test_anchor_bad_inputs_are_errors(tmp_path):
    p, a, _ = anchor_files(tmp_path, n=3)
    a.write_text(json.dumps({"idx": 99, "text": "x", "finish_reason": "stop"}) + "\n")
    with pytest.raises(D.E5DataError, match="bad answer row"):
        D.build_anchor_rows(p, a, 1)
    with pytest.raises(D.E5DataError):
        D.build_anchor_rows(tmp_path / "nope.jsonl", a, 1)


# ---- paraphrase scaffolding ----
def test_shipped_templates_pass_validate_shape_and_load():
    t = P.load_templates()
    P.validate_shape(t.by_relation)
    assert len(t.sha256) == 64 and all(len(v) == 2 for v in t.by_relation.values())


def test_load_refuses_an_empty_templates_file(tmp_path):
    f = tmp_path / "e.json"
    f.write_text(json.dumps({"templates": {}}))
    with pytest.raises(P.TemplatesNotWritten):
        P.load_templates(f)


def tpl_file(tmp_path, templates):
    f = tmp_path / "t.json"
    f.write_text(json.dumps({"_doc": "x", "templates": templates}))
    return f


def good_templates(items):
    return {i.prop: [f"Tell me about {{subj}} and {i.prop} (a)", f"{i.prop}? of {{subj}} (b)"] for i in items}


def test_templates_hash_is_checked_and_stable(tmp_path):
    t = {"genre": ["a {subj}", "b {subj}"]}
    f = tpl_file(tmp_path, t)
    h = P.templates_sha256(t)
    assert P.load_templates(f, h).sha256 == h and P.load_templates(f).sha256 == h
    assert P.templates_sha256({"genre": ["b {subj}", "a {subj}"]}) != h       # order is part of the hash
    with pytest.raises(P.E5DataError, match="edited after"):
        P.load_templates(f, "0" * 64)


def test_templates_loader_rejects_malformed_files(tmp_path):
    bad = tmp_path / "bad.json"
    bad.write_text("{nope")
    with pytest.raises(P.E5DataError):
        P.load_templates(bad)
    bad.write_text(json.dumps({"templates": []}))
    with pytest.raises(P.E5DataError, match="no `templates`"):
        P.load_templates(bad)


@pytest.mark.parametrize("tpls", [["only one {subj}"], ["same {subj}", "same {subj}"], ["no placeholder", "x {subj}"],
                                  ["{subj} {subj}", "x {subj}"], ["stray { brace {subj}", "x {subj}"]])
def test_template_shape_rules(tpls):
    with pytest.raises(P.E5DataError):
        P.validate_shape({"genre": tuple(tpls)})


def test_paraphrases_apply_to_the_first_200_eval_items_alternating_templates(small, tmp_path):
    items = popqa.load(write_tsv(tmp_path / "p.tsv", n_subj=400))
    ev = popqa.stratified_sample(popqa.unambiguous(items), 250, 1)
    sel = P.paraphrase_items(ev)
    assert sel == ev[:200]
    t = P.load_templates(tpl_file(tmp_path, good_templates(sel)))
    rows = P.apply_templates(sel, t)
    assert len(rows) == 200 and [r["template"] for r in rows[:4]] == [0, 1, 0, 1]
    assert all(r["question"] != r["original"] and sel[k].subj in rows[k]["question"] for k, r in enumerate(rows))
    assert P.apply_templates(sel, t) == rows
    with pytest.raises(P.E5DataError, match="10 eval items"):
        P.paraphrase_items(ev[:10], 250)


def test_template_equal_to_popqa_wording_or_missing_relation_is_refused(world, tmp_path):
    item = world[3][0]
    own = P.popqa_form(item)
    t = P.Templates({item.prop: (own, "other {subj}")}, "x")
    with pytest.raises(P.E5DataError, match="own wording"):
        P.apply_templates([item], t)
    with pytest.raises(P.E5DataError, match="no templates for relation"):
        P.apply_templates([item], P.Templates({"zzz": ("a {subj}", "b {subj}")}, "x"))
    assert P.popqa_form(Item("1", "Who?", "Nobody", "p", "o", ("o",), 1)) is None


# ---- CLI ----
def test_n_anchor_default_follows_n_lookup_requested():
    assert e5_build.default_n_anchor(1000) == 500 and e5_build.default_n_anchor(3000) == 1500
    assert e5_build.default_n_anchor(40) == 20


def test_describe_templates_shows_popqa_wording_beside_the_two_templates(world):
    ev = world[3]
    t = P.Templates({"genre": ("A {subj}?", "B {subj}?")}, "x")
    text = P.describe_templates(ev, t)
    assert "genre" in text and "popqa:" in text and "What is {subj}'s genre?" in text and "A {subj}?" in text
    assert "capital" in text and "NO TEMPLATES" in text            # relations in the sample without templates


def test_cli_builds_everything_and_refuses_a_missing_file(world, tmp_path, capsys):
    path, items, _, ev = world
    p, a, _ = anchor_files(tmp_path, n=30)
    t = tpl_file(tmp_path, good_templates(items))
    out = tmp_path / "cli"
    argv = ["--data", str(path), "--out", str(out), "--n-lookup", "40", "--anchor-prompts", str(p),
            "--anchor-answers", str(a), "--n-anchor", "20", "--templates", str(t)]
    # 80-item eval is below the 200 paraphrases P needs: the error is clean, not a traceback
    with pytest.raises(SystemExit, match="e5_build: .* eval items"):
        e5_build.main(argv)
    assert (out / "anchors_L1.jsonl").exists() and (out / "anchors_L0.jsonl").exists()
    out_text = capsys.readouterr().out
    assert "held out:" in out_text and "dropped_ungrounded:" in out_text and "popqa:" in out_text
    with pytest.raises(SystemExit, match="missing"):
        e5_build.main(["--data", str(tmp_path / "none.tsv"), "--out", str(out)])
    with pytest.raises(SystemExit):
        e5_build.main(["--out", str(out), "--anchor-prompts", str(p)])


def test_cli_paraphrase_step_writes_P(small, tmp_path, monkeypatch, capsys):
    path = write_tsv(tmp_path / "big.tsv", n_subj=500)
    monkeypatch.setattr(D, "EVAL_N", 250)
    items = popqa.load(path)
    t = tpl_file(tmp_path, good_templates(items))
    sha = P.templates_sha256(json.loads(t.read_text())["templates"])
    e5_build.main(["--data", str(path), "--out", str(tmp_path / "o"), "--n-lookup", "40",
                   "--templates", str(t), "--templates-sha256", sha])
    assert len((tmp_path / "o" / "paraphrase_P.jsonl").read_text().splitlines()) == 200
    assert sha in capsys.readouterr().out


# ---- the real file, when it is here ----
REAL = popqa.DEFAULT_PATH


@pytest.mark.skipif(not REAL.exists(), reason="data/popqa_test.tsv not fetched")
def test_real_popqa_eval_matches_committed_cell_and_split_is_disjoint():
    items, uri = popqa.load(REAL), D.load_s_uris(REAL)
    ev = D.eval_items(items)
    assert len(ev) == 1000 and len(items) == 14267
    same = D.check_against_cell(ev)
    assert same in (True, None)
    held = D.draw_heldout(ev)
    split = D.build_split(items, ev, held, uri)
    D.assert_disjoint(split, uri)
    assert len(split.dev) == 300 and held.eval_total >= 150 or held.below_threshold or len(held.attempts) == 2
    print(D.describe_draw(held, ev))
