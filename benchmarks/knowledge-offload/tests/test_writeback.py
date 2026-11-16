import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ko import conditions as C, runner, tools as T                      # noqa: E402
from test_harness import GOLD, FakeClient, call, fake_parallel, text    # noqa: E402


def ctx_with(replies):
    web = T.Web(None, post=fake_parallel()[0], backend="parallel")
    return C.Context(client=FakeClient(replies), web=web, notes=T.Notes(), variant="search_first")


def test_the_harness_writes_the_note_after_a_searched_answer_with_its_url():
    ctx = ctx_with([call("recall", query="q"), call("web_search", query="george rankin"), text("politician")])
    a = C.web_writeback(ctx, GOLD)
    assert a.extra["wrote"] and a.tainted and ctx.notes.count == 1
    note = ctx.notes.recall("George Rankin occupation")
    assert "Q: What is George Rankin's occupation? A: politician (per http://p)" in note


def test_an_answer_without_a_search_writes_nothing():
    ctx = ctx_with([call("recall", query="q"), text("politician")])
    a = C.web_writeback(ctx, GOLD)
    assert not a.extra["wrote"] and ctx.notes.count == 0


def test_a_wrong_answer_is_cached_too_and_the_model_has_no_remember_tool():
    ctx = ctx_with([call("web_search", query="q"), text("journalist")])
    C.web_writeback(ctx, GOLD)
    assert ctx.notes.count == 1
    offered = [t["function"]["name"] for t in ctx.client.calls[0]["tools"]]
    assert "remember" not in offered and set(offered) == {"recall", "web_search", "web_fetch"}


def test_pass_two_is_served_from_the_note_and_is_tainted_without_a_web_call():
    ctx = ctx_with([call("web_search", query="q"), text("politician"),            # pass 1
                    call("recall", query="George Rankin occupation"), text("politician")])   # pass 2
    res = runner.run_cell(condition="web+writeback", items=[GOLD], ctx=ctx, rep=0, meta={}, log=lambda *_: None)
    p1, p2 = [r for r in res["answers"] if r["pass"] == 1][0], [r for r in res["answers"] if r["pass"] == 2][0]
    assert p1["tools"] == ["web_search"] and p2["tools"] == ["recall"]
    assert p2["tainted"] and p2["contains"] and len(ctx.web.calls) == 1
    assert res["summary"]["pass1"]["searches_per_answer"] == 1.0 and res["summary"]["pass2"].get("searches_per_answer", 0) == 0


def test_web_writeback_is_not_in_the_default_condition_list():
    assert "web+writeback" not in C.CONDITIONS and "web+writeback" in C.EXTRA_CONDITIONS
    assert C.variant_for("web+writeback", "search_first") == "search_first"


def test_summarize_reports_pass2_against_pass1_and_the_search_ratio(capsys):
    import run
    rows = [{"id": str(i), "pass": p, "contains": True if p == 2 or i < 8 else False, "tools": ["web_search"] if p == 1 else ["recall"]}
            for i in range(10) for p in (1, 2)]
    cell = {"answers": rows, "summary": {"pass1": {"searches_per_answer": 1.0}, "pass2": {"searches_per_answer": 0.1}}}
    run.writeback_report({("t", "web+writeback/search_first"): [cell]})
    out = capsys.readouterr().out
    assert "pass2 - pass1: +0.200" in out and "1.00 -> 0.10 (ratio 0.10)" in out
