"""A search that answers with an error must stop the cell, not reach the model as tool text.

E1's 27B `web` cell lost 1661 of 2086 searches to this reply (results/payloads, read from yoda): the
JSON-RPC result carries `isError: true`; the harness raised, `tool_loop` turned the exception into
"error: ..." tool text, and the cell was written as if it had run.
"""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ko import conditions as C, tools as T                              # noqa: E402
from test_harness import GOLD, FakeClient, call, fake_parallel, text    # noqa: E402

# the exact `raw` text of every failed search in qwen3.5-27b-iq4_xs web-search_first-r0-p1 (1661 of them)
CREDIT_402 = ('{"jsonrpc":"2.0","id":2,"result":{"content":[{"text":"Error executing tool web_search: 402: '
              'Insufficient credit in account, please check your plan and billing details. You can add more funds '
              'from the billing page at https://platform.parallel.ai/settings?tab=billing and check for pricing '
              'details at https://platform.parallel.ai/pricing.","type":"text"}],"isError":true}}')


def post_with(reply):
    ok, _ = fake_parallel()

    def post(url, body, headers):
        if body.get("method") == "tools/call":
            return reply, {}
        return ok(url, body, headers)
    return post


def web_with(reply):
    return T.Web(None, post=post_with(reply), backend="parallel")


@pytest.mark.parametrize("reply", [
    CREDIT_402,
    "event: message\ndata: " + CREDIT_402 + "\n",
    json.dumps({"jsonrpc": "2.0", "id": 2, "result": {"content": [{"type": "text", "text": "not json at all"}]}}),
    json.dumps({"jsonrpc": "2.0", "id": 2, "error": {"code": -32000, "message": "boom"}}),
])
def test_an_error_reply_raises_search_failed_and_is_counted_and_recorded(reply):
    web = web_with(reply)
    with pytest.raises(T.SearchFailed):
        web.search("q")
    assert web.search_errors == 1 and web.tavily_calls == 0
    assert len(web.error_log) == 1 and web.error_log[0]["text"]


def test_the_402_text_and_class_are_kept_for_the_result_json():
    web = web_with(CREDIT_402)
    with pytest.raises(T.SearchFailed):
        web.search("q")
    assert "402" in web.error_log[0]["text"] and "Insufficient credit" in web.error_log[0]["text"]
    assert web.error_log[0]["status"] == 402


def test_search_failed_stops_the_tool_loop_instead_of_reaching_the_model():
    ctx = C.Context(client=FakeClient([call("web_search", query="q"), text("never reached")]), web=web_with(CREDIT_402))
    with pytest.raises(T.SearchFailed):
        C.web(ctx, GOLD)


def test_a_network_failure_is_also_a_stop_not_text_for_the_model():
    post, _ = fake_parallel(fail=True)
    ctx = C.Context(client=FakeClient([call("web_search", query="q"), text("never reached")]),
                    web=T.Web(None, post=post, backend="parallel"))
    with pytest.raises(T.SearchFailed):
        C.web(ctx, GOLD)


def test_search_failed_and_blocked_are_both_search_stops():
    assert issubclass(T.SearchFailed, T.SearchStop) and issubclass(T.SearchBlocked, T.SearchStop)
