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


# ---- 2026-11-15 additions: payloads, hedge, shared pool, stratified sample, buckets ----
import gzip                                                             # noqa: E402
import importlib.util                                                   # noqa: E402
import json                                                             # noqa: E402

from ko import popqa, runner                                            # noqa: E402
from ko.scoring import named_candidates                                 # noqa: E402

CANDS = (("politician", "political leader"), ("journalist",), ("Georgia",),
         ("Georgia Institute of Technology",))


def it(i, subj, prop, obj, pop, aliases=None):
    return Item(str(i), f"What is {subj}'s {prop}?", subj, prop, obj, tuple(aliases or [obj]), pop)


def test_payloads_keep_the_raw_reply_and_both_attempts_on_fallback():
    post, _ = fake_parallel()
    web = T.Web(None, post=post)
    web.search("george rankin")
    p = web.payloads[0]
    assert p["kind"] == "search" and p["backend"] == "parallel"
    assert p["attempts"][0]["backend"] == "parallel" and "politician" in p["attempts"][0]["raw"]
    post, _ = fake_parallel(fail=True)
    web = T.Web("k", post=post, transport=lambda url, body=None: '{"results":[{"title":"t","url":"http://x","content":"c"}]}')
    web.search("q")
    kinds = [(x["backend"], "raw" in x or "error" in x) for x in web.payloads[0]["attempts"]]
    assert ("parallel", True) in kinds and ("tavily", True) in kinds     # the failure and the answer
    web.fetch("http://x/page")
    assert web.payloads[-1] == {"kind": "fetch", "url": "http://x/page", "raw": web.transport("http://x/page")}


def test_web_answer_carries_payloads_and_the_run_hands_them_on(tmp_path):
    post, _ = fake_parallel()
    web = T.Web(None, post=post)
    c = C.Context(client=FakeClient([call("web_search", query="q"), text("politician")]), web=web)
    a = C.web(c, GOLD)
    assert len(a.payloads) == 1 and web.payloads == []                  # taken, not kept in memory
    runner.write_payloads(tmp_path / "web-r0-p1", GOLD.id, a.payloads)
    got = json.loads(gzip.open(tmp_path / "web-r0-p1" / "1.json.gz", "rt").read())
    assert got["id"] == "1" and got["payloads"][0]["attempts"][0]["raw"]
    runner.write_payloads(tmp_path / "none", "9", [])                   # nothing to keep, no empty file
    assert not (tmp_path / "none").exists()


def test_payload_dir_is_keyed_by_run_condition_repeat_and_pass():
    d = runner.payload_dir("t9b", "web+cache", 1, 2)
    assert d.parts[-3:] == ("payloads", "t9b", "webpluscache-r1-p2")
    assert "strat1" in str(runner.payload_dir("t9b", "web", 0, 1, sample="strat", dataset_seed=1))


def test_hedge_is_two_candidate_objects_named_not_a_substring():
    assert named_candidates("politician", CANDS) == 1
    assert named_candidates("He was a politician and a journalist", CANDS) == 2
    assert named_candidates("Georgia Institute of Technology", CANDS) == 1    # not also "Georgia"
    assert named_candidates("Georgia or Georgia Institute of Technology", CANDS) == 2
    assert named_candidates("", CANDS) == 0
    s = score("politician or journalist", ["politician"], CANDS[:2])
    assert s["contains"] and s["hedge"]
    assert not score("politician", ["politician"], CANDS[:2])["hedge"]
    assert "hedge" not in score("politician", ["politician"])                 # old call sites unchanged


def test_candidate_sets_group_the_homonyms_of_one_subject_and_property():
    items = [it(1, "Paris", "country", "France", 9), it(2, "Paris", "country", "United States", 3),
             it(3, "Paris", "occupation", "city", 1), it(4, "Paris", "country", "France", 2)]
    c = popqa.candidate_sets(items)
    assert len(c[("Paris", "country")]) == 2          # France once, not twice
    assert len(c[("Paris", "occupation")]) == 1


def test_run_cell_records_hedge_and_the_summary_reports_it_adjusted(tmp_path):
    items = [GOLD, it(2, "John Mayne", "occupation", "journalist", 50)]
    cand = popqa.candidate_sets(items + [it(3, "John Mayne", "occupation", "poet", 5)])
    c = C.Context(client=FakeClient([text("journalist or poet"), text("politician")]),
                  store=FactStore(FACTS))
    res = runner.run_cell(condition="closed", items=[items[1], items[0]], ctx=c, rep=0,
                          meta={"model_tag": "t"}, candidates=cand, log=lambda *_: None)
    assert [r["hedge"] for r in res["answers"]] == [True, False]
    s = res["summary"]["pass1"]
    assert s["acc"] == 1.0 and s["acc_nohedge"] == 0.5 and s["hedge_rate"] == 0.5


def test_one_triple_list_feeds_store_and_funes_pool():
    items = [it(1, "Alpha Town", "country", "x", 1), it(2, "Beta Town", "occupation", "y", 2)]
    assert runner.pool_texts(items) == [Fact(*t).text() for t in popqa.facts(items)]
    assert [f.text() for f in runner.build_store(items).search("Alpha Town country", k=5)][0] == \
        "Alpha Town | country | x"
    assert runner.pool_texts(items)[0] == "Alpha Town | country | x"


def test_stratified_sample_equalises_buckets_caps_small_ones_and_is_deterministic():
    items = ([it(i, f"a{i}", "p", "x", 5) for i in range(200)]               # 1e0
             + [it(1000 + i, f"b{i}", "p", "x", 50) for i in range(200)]     # 1e1
             + [it(2000 + i, f"c{i}", "p", "x", 5000) for i in range(3)])    # 1e3: only 3
    s = popqa.stratified_sample(items, 30, seed=1)
    by = {}
    for x in s:
        by[popqa.pop_bucket(x)] = by.get(popqa.pop_bucket(x), 0) + 1
    assert len(s) == 30 and by["1e3"] == 3 and abs(by["1e0"] - by["1e1"]) <= 1 and by["1e0"] + by["1e1"] == 27   # shortfall spread
    assert [x.id for x in s] == [x.id for x in popqa.stratified_sample(items, 30, seed=1)]
    assert [x.id for x in s] != [x.id for x in popqa.stratified_sample(items, 30, seed=2)]
    assert len({x.id for x in s}) == 30


def test_cell_names_keep_e0_files_and_new_rows_do_not_overwrite_them():
    old = runner.cell_path("t", 1000, "funes", 0)
    assert old.name == "popqa-n1000-t-funes-r0.json"                          # E0's own file
    assert runner.cell_path("t", 1000, "funes", 0, pool=14267).name == "popqa-n1000-t-funes-pool14267-r0.json"
    assert runner.cell_path("t", 1000, "store", 0, pool=14267).name == "popqa-n1000-t-store-r0.json"
    assert runner.cell_path("t", 1000, "closed", 0, sample="stratified", dataset_seed=1).name == \
        "popqa-stratified1-n1000-t-closed-r0.json"


def test_funes_pool_is_copied_per_run_and_logged_into_not_bootstrapped(tmp_path):
    from ko.funes import FunesInstance
    binary = tmp_path / "funes"
    binary.write_text("")
    pool = tmp_path / "pool"
    pool.mkdir()
    (pool / "memory.db").write_bytes(b"db")
    (pool / "memory.db-wal").write_bytes(b"wal")
    (pool / "account.json").write_text(json.dumps({"username": "ko_abc", "password": "pw"}))
    inst = FunesInstance("http://x", "http://y", binary, pool=pool)
    assert (inst.username, inst._password) == ("ko_abc", "pw")
    # save_pool is what builds one: it folds the scratch db and the account into a directory
    (inst.scratch / "memory.db").write_bytes(b"built")
    out = inst.save_pool(tmp_path / "saved")
    assert (out / "memory.db").read_bytes() == b"built"
    assert json.loads((out / "account.json").read_text())["username"] == "ko_abc"
    import shutil
    shutil.rmtree(inst.scratch, ignore_errors=True)


def load_run_module():
    spec = importlib.util.spec_from_file_location("ko_run", Path(__file__).resolve().parent.parent / "run.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_summarize_labels_funes_pools_and_samples_and_computes_hedge_for_old_cells():
    run = load_run_module()
    assert run.cell_label({"condition": "funes", "funes_pool": 3000}) == "funes@3000"
    assert run.cell_label({"condition": "store"}) == "store"
    assert run.cell_label({"condition": "store", "sample": "stratified", "dataset_seed": 1}) == "store [stratified s1]"
    items = [it(1, "P", "p", "x", 1), it(2, "P", "p", "y", 1)]
    cand, by_id = popqa.candidate_sets(items), {i.id: i for i in items}
    r = {"answers": [{"id": "1", "prediction": "x or y"}, {"id": "2", "prediction": "y"}]}
    run.with_hedge(r, cand, by_id)
    assert [a["hedge"] for a in r["answers"]] == [True, False]


def test_by_bucket_table_counts_n_per_bucket(capsys):
    run = load_run_module()
    ans = [{"pass": 1, "pop_bucket": "1e1", "contains": True}, {"pass": 1, "pop_bucket": "1e1", "contains": False},
           {"pass": 1, "pop_bucket": "1e3", "contains": True}]
    run.by_bucket({("t", "store"): [{"answers": ans}]})
    out = capsys.readouterr().out
    assert "0.500 [2]" in out and "1.000 [1]" in out


def test_prompt_variant_changes_only_the_system_prompt_and_names_the_row():
    for cond in ("tool", "tool+contract", "web", "web+cache"):
        base, sf = C.Context(), C.Context(variant="search_first")
        assert C.system_for(base, "x") == C.SYSTEM_TOOL
        assert "MUST first call x" in C.system_for(sf, "x") and "short phrase" in C.system_for(sf, "x")
    c = C.Context(client=FakeClient([call("lookup", entity="George Rankin"), text("politician")]),
                  store=FactStore(FACTS), variant="search_first")
    C.tool(c, GOLD)
    assert "MUST first call the lookup tool" in c.client.calls[0]["messages"][0]["content"]
    assert runner.cell_path("t", 1000, "tool", 0).name == "popqa-n1000-t-tool-r0.json"      # old file untouched
    assert runner.cell_path("t", 1000, "tool", 0, variant="search_first").name == "popqa-n1000-t-tool-search_first-r0.json"
    assert runner.payload_dir("t", "web", 0, 1, variant="search_first").name == "web-search_first-r0-p1"
    run = load_run_module()
    assert run.cell_label({"condition": "web+cache", "prompt_variant": "search_first"}) == "web+cache/search_first"
    assert run.cell_label({"condition": "tool"}) == "tool"


def test_tavily_is_never_spent_past_its_cap():
    web = T.Web("k", backend="tavily", transport=lambda url, body=None: '{"results":[]}')
    web.TAVILY_CAP = 2
    web.search("a"); web.search("b")
    with pytest.raises(RuntimeError, match="cap"):
        web.search("c")
    assert web.tavily_calls == 2


def test_parallel_429_is_waited_out_and_paced_before_tavily_is_spent():
    import urllib.error
    post, log = fake_parallel()
    state = {"n": 0}

    def flaky(url, body=None, headers=None):
        if body and body.get("method") == "tools/call":
            state["n"] += 1
            if state["n"] <= 2:
                raise urllib.error.HTTPError(url, 429, "Too Many Requests", {}, None)
        return post(url, body, headers)
    web = T.Web(None, post=flaky)
    slept = []
    web.sleep = slept.append
    out = web.search("george rankin")
    assert "politician" in out and web.calls == [{"backend": "parallel"}]
    assert state["n"] == 3 and [x for x in slept if x >= 5] == [5, 15] and web.rate_limited == 2 and web.MIN_GAP == 2.0


def test_parallel_429_beyond_the_backoff_falls_back():
    import urllib.error

    def always(url, body=None, headers=None):
        if body and body.get("method") == "tools/call":
            raise urllib.error.HTTPError(url, 429, "x", {}, None)
        return fake_parallel()[0](url, body, headers)
    web = T.Web("k", post=always, transport=lambda u, b=None: '{"results":[]}')
    web.sleep = lambda s: None
    web.search("q")
    assert web.calls[0]["backend"] == "tavily" and "429" in web.calls[0]["fallback_reason"]
