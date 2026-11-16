import sys
import urllib.error
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ko import conditions as C, tools as T                              # noqa: E402
from test_harness import GOLD, FakeClient, call, fake_parallel, text    # noqa: E402


def always_429(url, body=None, headers=None):
    if body and body.get("method") == "tools/call":
        raise urllib.error.HTTPError(url, 429, "Too Many Requests", {}, None)
    return fake_parallel()[0](url, body, headers)


def parallel_only():
    web = T.Web(None, post=always_429, backend="parallel")
    web.sleep = lambda s: None
    return web


def test_exhausted_429_without_tavily_raises_search_blocked_and_counts_them():
    web = parallel_only()
    with pytest.raises(T.SearchBlocked):
        web.search("q")
    assert web.rate_limited == 1 + len(web.BACKOFF) and web.tavily_calls == 0
    assert web.calls[0]["backend"] == "none"


def test_search_blocked_stops_the_tool_loop_instead_of_reaching_the_model():
    web = parallel_only()
    ctx = C.Context(client=FakeClient([call("web_search", query="q"), text("never reached")]), web=web)
    with pytest.raises(T.SearchBlocked):
        C.web(ctx, GOLD)


def test_other_search_failures_are_still_tool_errors_the_model_sees():
    post, _ = fake_parallel(fail=True)           # an OSError, not a 429
    web = T.Web(None, post=post, backend="parallel")
    client = FakeClient([call("web_search", query="q"), text("politician")])
    a = C.web(C.Context(client=client, web=web), GOLD)
    assert a.text == "politician"
    assert any("error:" in m["content"] for m in client.calls[1]["messages"] if m["role"] == "tool")

