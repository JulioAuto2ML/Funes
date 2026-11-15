import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ko import conditions as C, stats, tools as T                      # noqa: E402
from ko.llm import Client, LLMError, Reply, Usage                       # noqa: E402
from ko.popqa import Item, pop_bucket, sample                           # noqa: E402
from ko.scoring import score                                            # noqa: E402
from ko.store import Fact, FactStore                                    # noqa: E402

GOLD = Item("1", "What is George Rankin's occupation?", "George Rankin", "occupation",
            "politician", ("politician", "political leader"), 142)
FACTS = [Fact("George Rankin", "occupation", "politician"),
         Fact("George Rankin", "place of birth", "Sydney"),
         Fact("John Mayne", "occupation", "journalist")]


class FakeClient:
    """Scripted replies; records what each call was given."""
    def __init__(self, replies):
        self.replies, self.calls = list(replies), []

    def chat(self, messages, tools=None, **kw):
        self.calls.append({"messages": [dict(m) for m in messages], "tools": tools})
        return self.replies.pop(0)


def text(t):
    return Reply(text=t, usage=Usage(prompt_tokens=10, completion_tokens=2, calls=1))


def call(name, **args):
    return Reply(text="", tool_calls=[{"id": "c1", "name": name, "arguments": args}],
                 raw_message={"role": "assistant", "content": None, "tool_calls": []},
                 usage=Usage(calls=1))


def ctx(replies, store=None):
    return C.Context(client=FakeClient(replies), store=store or FactStore(FACTS))


def test_scoring_alias_and_normalisation():
    assert score("The Politician.", ["politician"])["exact"]
    assert score("He was a politician", ["politician"])["contains"]
    assert not score("He was a politician", ["politician"])["exact"]
    assert not score("apolitical", ["politician"])["contains"]      # word boundary
    assert score("I don't know", ["x"])["abstained"]
    assert score("Zoë", ["Zoe"])["exact"]


def test_store_search_and_lookup():
    s = FactStore(FACTS)
    assert s.search(GOLD.question, k=1)[0].obj == "politician"
    assert [f.obj for f in s.lookup("george rankin", "occupation")] == ["politician"]
    assert s.lookup("Georgie Rankin")           # fuzzy fallback instead of a typo = miss
    assert s.search("") == []


def test_closed_and_oracle_prompts():
    c = ctx([text("politician"), text("politician")])
    C.closed(c, GOLD)
    C.oracle(c, GOLD)
    assert "Facts" not in c.client.calls[0]["messages"][1]["content"]
    assert "George Rankin | occupation | politician" in c.client.calls[1]["messages"][1]["content"]


def test_store_records_retrieval_hit():
    c = ctx([text("politician")])
    a = C.store(c, GOLD)
    assert a.retrieval_hit is True and a.text == "politician"


def test_tool_condition_lets_model_skip_lookup():
    a = C.tool(ctx([text("a politician")]), GOLD)
    assert a.tools == [] and a.contract_met is None


def test_contract_refuses_text_until_lookup_succeeds():
    c = ctx([text("guess"), call("lookup", entity="George Rankin", relation="occupation"),
             text("politician")])
    a = C.tool_contract(c, GOLD)
    assert a.nudges == 1 and a.tools == ["lookup"] and a.contract_met is True
    assert a.text == "politician"
    assert "lookup" in c.client.calls[1]["messages"][-1]["content"]


def test_contract_gives_up_after_max_nudges_and_flags_it():
    a = C.tool_contract(ctx([text("x")] * (C.MAX_NUDGES + 1)), GOLD)
    assert a.contract_met is False and a.nudges == C.MAX_NUDGES


def test_last_step_withholds_tools():
    c = ctx([call("lookup", entity="x")] * C.MAX_STEPS + [text("final")])
    a = C.tool(c, GOLD)
    assert a.text == "final" and c.client.calls[-1]["tools"] is None


def test_tool_error_is_data_not_a_crash():
    c = ctx([call("nope"), text("done")])
    assert C.tool(c, GOLD).text == "done"


def test_web_unavailable_without_key_is_refused_not_faked():
    for fn in (C.web, C.web_cache):
        with pytest.raises(C.ConditionUnavailable):
            fn(C.Context(client=FakeClient([]), web=T.Web(None, backend="tavily")), GOLD)
    with pytest.raises(C.ConditionUnavailable):
        C.funes(C.Context(), GOLD)


def test_web_marks_taint_and_counts_searches():
    web = T.Web("k", backend="tavily", transport=lambda url, body=None:
                '{"results":[{"title":"t","url":"http://x","content":"politician"}]}')
    c = C.Context(client=FakeClient([call("web_search", query="q"), text("politician")]), web=web)
    a = C.web(c, GOLD)
    assert a.tainted and web.searches == 1


def test_web_cache_second_pass_recalls():
    notes = T.Notes()
    notes.remember("George Rankin is a politician (http://x)")
    web = T.Web("k", backend="tavily")
    c = C.Context(client=FakeClient([call("recall", query="George Rankin"), text("politician")]),
                  web=web, notes=notes, passno=2)
    a = C.web_cache(c, GOLD)
    assert a.tools == ["recall"] and a.tainted and web.searches == 0 and a.extra["pass"] == 2


def test_local_endpoint_refused():
    with pytest.raises(LLMError):
        Client("http://localhost:8080")
    Client("http://yoda:8080")
    Client("http://localhost:8080", allow_local=True)


def test_stats():
    lo, hi = stats.bootstrap_ci([1.0] * 50 + [0.0] * 50, resamples=2000)
    assert 0.38 < lo < 0.5 < hi < 0.62
    d, _ = stats.paired_bootstrap([1, 1, 0, 1], [0, 0, 0, 1], resamples=500)
    assert d == 0.5
    assert stats.mcnemar([True] * 10 + [False] * 10, [False] * 10 + [True] * 10)["p"] == 1.0
    assert stats.mcnemar([True] * 20, [False] * 20)["p"] < 1e-4
    assert stats.spread([0.5, 0.52, 0.48])["range"] == pytest.approx(0.04)


def test_sample_is_deterministic_and_bucketed():
    items = [Item(str(i), "q", "s", "p", "o", ("o",), 10 ** (i % 5)) for i in range(100)]
    assert sample(items, 10, 0) == sample(items, 10, 0) != sample(items, 10, 1)
    assert pop_bucket(items[3]) == "1e3"


def test_web_cache_answer_from_a_note_is_tainted():
    notes = T.Notes()
    notes.remember("George Rankin is a politician (http://x)")
    c = C.Context(client=FakeClient([call("recall", query="George Rankin"), text("politician")]),
                  web=T.Web("k", backend="tavily"), notes=notes, passno=2)
    assert C.web_cache(c, GOLD).tainted


import json as _json
_INNER = {"results": [{"url": "http://p", "title": "\n\t T \t", "publish_date": None,
                       "excerpts": ["George  Rankin\nwas a politician"]}]}
PARALLEL_BODY = _json.dumps({"jsonrpc": "2.0", "id": 2, "result": {
    "content": [{"type": "text", "text": _json.dumps(_INNER)}]}})


def fake_parallel(fail=False):
    log = []

    def post(url, body, headers):
        log.append(body.get("method"))
        if body.get("method") == "initialize":
            return '{"jsonrpc":"2.0","id":1,"result":{}}', {"Mcp-Session-Id": "S1"}
        if body.get("method") == "notifications/initialized":
            return "", {}
        assert headers["Mcp-Session-Id"] == "S1"
        if fail:
            raise OSError("429 Too Many Requests")
        return "event: message\ndata: " + PARALLEL_BODY + "\n", {}
    return post, log


def test_parallel_first_session_reused_and_backend_recorded():
    post, log = fake_parallel()
    web = T.Web(None, post=post)
    out = web.search("george rankin")
    assert "T | http://p | George Rankin was a politician" in out
    web.search("again")
    assert log.count("initialize") == 1 and log.count("tools/call") == 2
    assert web.calls == [{"backend": "parallel"}, {"backend": "parallel"}] and web.available


def test_parallel_failure_falls_back_to_tavily_and_says_so():
    post, _ = fake_parallel(fail=True)
    web = T.Web("k", post=post, transport=lambda url, body=None:
                '{"results":[{"title":"t","url":"http://x","content":"c"}]}')
    web.search("q")
    assert web.calls[0]["backend"] == "tavily" and "429" in web.calls[0]["fallback_reason"]


def test_parallel_failure_without_tavily_key_raises_and_is_recorded():
    post, _ = fake_parallel(fail=True)
    web = T.Web(None, post=post)
    with pytest.raises(RuntimeError):
        web.search("q")
    assert web.calls[0]["backend"] == "none"


def test_web_condition_puts_backends_in_the_answer():
    post, _ = fake_parallel()
    web = T.Web(None, post=post)
    c = C.Context(client=FakeClient([call("web_search", query="q"), text("politician")]), web=web)
    a = C.web(c, GOLD)
    assert a.extra["search_backends"] == [{"backend": "parallel"}] and a.tainted
