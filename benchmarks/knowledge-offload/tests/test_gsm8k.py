"""GSM8K arm: parser cases from the design (§6), sampler, file validation, cell names. No network, no model."""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

import pytest

from ko import gsm8k as G


def ext(text, fr="stop"):
    raw, mode, val = G.extract(text, fr)
    return mode, val


def test_strict_with_comma():
    assert ext("so\n#### 1,234") == ("strict", 1234.0)

def test_strict_trailing_dot():
    assert ext("#### 18.") == ("strict", 18.0)

def test_dollar_sign_after_hashes_is_fallback_but_right():
    mode, val = ext("#### $5")
    assert (mode, val) == ("fallback", 5.0)

def test_two_hash_lines_last_wins():
    assert ext("#### 3\nwait\n#### 4") == ("strict", 4.0)

def test_no_hashes_uses_last_number():
    assert ext("so the answer is 42.") == ("fallback", 42.0)

def test_arithmetic_without_final_line():
    assert ext("3 + 4 = 7") == ("fallback", 7.0)

def test_empty_reply_is_none():
    assert ext("") == ("none", None)

def test_negative():
    assert ext("#### -3") == ("strict", -3.0)

def test_percent_fallback():
    assert ext("it is 50%") == ("fallback", 50.0)

def test_length_without_hashes_is_none_even_with_numbers():
    assert ext("1 + 2 = 3 and then", fr="length") == ("none", None)

def test_length_with_hashes_still_strict():
    assert ext("#### 9", fr="length") == ("strict", 9.0)

def test_no_number_is_none():
    assert ext("I do not know") == ("none", None)

def test_correct_tolerance_and_none_is_wrong():
    assert G.is_correct(18.0, 18.0)
    assert not G.is_correct(18.1, 18.0)
    assert not G.is_correct(None, 18.0)

def test_score_strict_only_flag():
    assert G.score("#### 5", "stop", 5.0)["correct_strict"]
    s = G.score("the answer is 5", "stop", 5.0)
    assert s["correct"] and not s["correct_strict"] and s["mode"] == "fallback"


def write_file(path, n=G.N_PROBLEMS, bad=None):
    with open(path, "w") as f:
        for i in range(n):
            ans = f"work {i}\n#### {i}" if bad != i else "no marker"
            f.write(json.dumps({"question": f"q{i}", "answer": ans}) + "\n")


def test_validate_ok_and_counts(tmp_path):
    p = tmp_path / "t.jsonl"; write_file(p)
    assert G.validate(p) == G.N_PROBLEMS

def test_validate_rejects_wrong_count(tmp_path):
    p = tmp_path / "t.jsonl"; write_file(p, n=10)
    with pytest.raises(ValueError):
        G.validate(p)

def test_validate_rejects_answer_without_marker(tmp_path):
    p = tmp_path / "t.jsonl"; write_file(p, bad=7)
    with pytest.raises(ValueError):
        G.validate(p)

def test_load_gold_and_ids(tmp_path):
    p = tmp_path / "t.jsonl"; write_file(p)
    ps = G.load(p)
    assert ps[5].id == "gsm8k-test-5" and ps[5].gold == 5.0 and ps[5].question == "q5"

def test_sample_deterministic_and_pilot_disjoint(tmp_path):
    p = tmp_path / "t.jsonl"; write_file(p)
    ps = G.load(p)
    a, b = G.sample(ps), G.sample(ps)
    assert [x.id for x in a] == [x.id for x in b] and len(a) == 500 and len({x.id for x in a}) == 500
    pilot = G.pilot_sample(ps)
    assert len(pilot) == 20 and not ({x.id for x in pilot} & {x.id for x in a})

def test_pinned_hash_gate(tmp_path, monkeypatch):
    p = tmp_path / "t.jsonl"; write_file(p)
    monkeypatch.setattr(G, "EXPECTED_SHA256", None)
    with pytest.raises(ValueError):
        G.check_pinned(p)
    monkeypatch.setattr(G, "EXPECTED_SHA256", "0" * 64)
    with pytest.raises(ValueError):
        G.check_pinned(p)
    monkeypatch.setattr(G, "EXPECTED_SHA256", G.sha256(p))
    assert G.check_pinned(p) == G.sha256(p)

def test_cell_names():
    r = Path("results")
    assert G.cell_path(r, "qwen3.5-9b-iq4_xs", "closed", 0).name == "gsm8k-n500-s1-qwen3.5-9b-iq4_xs-closed-r0.json"
    assert "pilot" in G.cell_path(r, "t", "closed", 0, n=20, seed=2, pilot=True).name


class FakeClient:
    """Answers a fixed text; records the messages it was given."""
    def __init__(self, text="x\n#### 3", finish="stop"):
        self.text, self.finish, self.seen = text, finish, []
    def chat(self, messages, tools=None, tool_choice=None, seed=0, max_tokens=256, temperature=0.0):
        from ko.llm import Reply, Usage
        self.seen.append((messages, max_tokens, tools))
        return Reply(text=self.text, usage=Usage(completion_tokens=7, calls=1), finish_reason=self.finish)


class FakeStore:
    def search(self, q, k=5, exclude=None):
        from ko.store import Fact
        return [Fact("Paris", "country", "France")]


def make_ctx(client):
    from ko import conditions as C
    ctx = C.Context(client=client, store=FakeStore(), seed=0)
    ctx.max_tokens = 512
    return ctx


def test_closed_and_store_differ_only_by_facts_block(tmp_path):
    p = tmp_path / "t.jsonl"; write_file(p)
    probs = G.load(p)[:2]
    c1, c2 = FakeClient(), FakeClient()
    r1 = G.run_cell(cond="closed", problems=probs, ctx=make_ctx(c1), rep=0, meta={}, log=lambda *_: None)
    r2 = G.run_cell(cond="store", problems=probs, ctx=make_ctx(c2), rep=0, meta={}, log=lambda *_: None)
    m1, m2 = c1.seen[0][0], c2.seen[0][0]
    assert m1[0] == m2[0] and m1[0]["content"] == G.SYSTEM            # same system prompt
    assert m1[1]["content"] == "q0"
    assert m2[1]["content"].startswith("Facts:\n- ") and m2[1]["content"].endswith("\n\nQuestion: q0")
    assert c1.seen[0][1] == c2.seen[0][1] == 512                       # same max_tokens
    assert r2["answers"][0]["facts"] and r1["answers"][0]["facts"] == []
    assert r1["summary"]["n"] == 2 and r1["summary"]["errors"] == 0


def test_summary_counts_modes_limit_and_flag(tmp_path):
    p = tmp_path / "t.jsonl"; write_file(p)
    probs = G.load(p)[:5]
    res = G.run_cell(cond="closed", problems=probs, ctx=make_ctx(FakeClient("no digits here", "length")),
                     rep=0, meta={}, log=lambda *_: None)
    s = res["summary"]
    assert s["acc"] == 0 and s["modes"]["none"] == 1.0 and s["at_limit"] == 1.0 and s["format_dominated"]
    assert s["completion_tokens_mean"] == 7


def test_tool_condition_uses_tools_and_extra_system(tmp_path):
    p = tmp_path / "t.jsonl"; write_file(p)
    c = FakeClient()
    ctx = make_ctx(c)
    res = G.run_cell(cond="tool", problems=G.load(p)[:1], ctx=ctx, rep=0, meta={}, log=lambda *_: None)
    msgs, mt, tools = c.seen[0]
    assert msgs[0]["content"] == G.SYSTEM_TOOL and tools and mt == 512
    assert res["answers"][0]["finish_reason"] == "stop"
