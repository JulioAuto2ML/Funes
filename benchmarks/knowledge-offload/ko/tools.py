"""Tool schemas and executors the conditions hand to the model."""
import html
import json
import os
import re
import time
import urllib.error
import urllib.request
from pathlib import Path

from .store import Fact, FactStore

LOOKUP = {"type": "function", "function": {
    "name": "lookup",
    "description": "Look up stored facts about an entity. Returns lines 'entity | relation | value'.",
    "parameters": {"type": "object", "properties": {
        "entity": {"type": "string", "description": "the subject, e.g. a person or a place"},
        "relation": {"type": "string", "description": "the attribute asked for, e.g. occupation, country"}},
        "required": ["entity"]}}}

WEB_SEARCH = {"type": "function", "function": {
    "name": "web_search", "description": "Search the web. Returns titles, URLs and snippets.",
    "parameters": {"type": "object", "properties": {"query": {"type": "string"}},
                   "required": ["query"]}}}
WEB_FETCH = {"type": "function", "function": {
    "name": "web_fetch", "description": "Fetch a web page and return its text.",
    "parameters": {"type": "object", "properties": {"url": {"type": "string"}},
                   "required": ["url"]}}}
RECALL = {"type": "function", "function": {
    "name": "recall", "description": "Search your own notes of facts you looked up earlier.",
    "parameters": {"type": "object", "properties": {"query": {"type": "string"}},
                   "required": ["query"]}}}
REMEMBER = {"type": "function", "function": {
    "name": "remember", "description": "Save one short, self-contained fact (with its source URL) for later.",
    "parameters": {"type": "object", "properties": {"fact": {"type": "string"}},
                   "required": ["fact"]}}}


def run_lookup(store: FactStore, args: dict, exclude: Fact | None = None) -> str:
    facts = store.lookup(str(args.get("entity", "")), str(args.get("relation", "")), exclude=exclude)
    return "\n".join(f.text() for f in facts) or "no stored facts match"


def _config(name: str) -> str | None:
    """Env first, then the checkout's own config/funes.local — the file Funes
    itself reads. Never printed, never written to a result."""
    if os.environ.get(name):
        return os.environ[name]
    cfg = Path(__file__).resolve().parents[3] / "config" / "funes.local"
    if cfg.exists():
        m = re.search(rf"^\s*(?:export\s+)?{name}\s*=\s*(\S+)", cfg.read_text(), re.M)
        if m:
            return m.group(1).strip("'\"")
    return None


def tavily_key() -> str | None:
    return _config("FUNES_TAVILY_API_KEY") or os.environ.get("TAVILY_API_KEY")


PARALLEL_URL = "https://search.parallel.ai/mcp"


def _http(url: str, body: dict | None = None, headers: dict | None = None):
    req = urllib.request.Request(
        url, data=json.dumps(body).encode() if body else None,
        headers={"Content-Type": "application/json", "User-Agent": "funes-ko-bench/0",
                 **(headers or {})})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read(2_000_000).decode("utf-8", "replace"), dict(r.headers)


def _mcp_json(text: str) -> dict:
    """A streamable-HTTP reply is plain JSON or an SSE stream of `data:` lines."""
    text = text.strip()
    if text.startswith("{"):
        return json.loads(text)
    for line in text.splitlines():
        if line.startswith("data:") and line[5:].strip().startswith("{"):
            return json.loads(line[5:])
    raise ValueError("no JSON-RPC message in reply")


class SearchStop(RuntimeError):
    """The search backend cannot answer. Never a tool result for the model: the run stops, the
    unfinished cell is not written, and a later run redoes it. (E1's 27B `web` cell was written
    with 1661 of 2086 searches failed because this was only ever tool text.)"""


class SearchFailed(SearchStop):
    """Parallel answered with an error (HTTP or MCP `isError`, e.g. `402: Insufficient credit`), a body
    that is not a result list, or not at all, and there is no other backend. `status` is the HTTP/MCP
    code when one could be read."""

    def __init__(self, message: str, status: int | None = None):
        super().__init__(message)
        self.status = status


def error_status(e: BaseException) -> int | None:
    """The HTTP status of a failed search: urllib's own, else the `NNN:` an MCP error text opens with."""
    code = getattr(e, "code", None)
    if isinstance(code, int):
        return code
    m = re.search(r"\b([1-5]\d\d):", str(e))
    return int(m.group(1)) if m else None


class SearchBlocked(SearchStop):
    """Parallel kept answering 429 through the whole backoff and there is no
    other backend to spend. Not a tool failure to show the model: the run stops,
    the unfinished cell is not written, and a later run redoes it."""


class Web:
    """web_search and web_fetch, as Funes's own web_search does it: Parallel's
    free search MCP first (FUNES_SEARCH_BACKEND=parallel, keyless), Tavily as the
    fallback when Parallel errors or rate-limits. Which backend answered each
    call is kept in `calls` for the result JSON. No synthesised answer is asked
    of either, so the engine isn't answering for the model.

    `post(url, body, headers) -> (text, response_headers)` and
    `transport(url, body) -> text` are swappable: the unit tests never touch
    the network."""

    def __init__(self, key: str | None = None, transport=None, post=None,
                 backend: str | None = None, parallel_key: str | None = None,
                 parallel_url: str = PARALLEL_URL, max_chars: int = 4000):
        self.key, self.max_chars = key, max_chars
        self.post = post or _http
        self.transport = transport or (lambda url, body=None: _http(url, body)[0])
        self.backend = backend or _config("FUNES_SEARCH_BACKEND") or "parallel"
        self.parallel_key = parallel_key if parallel_key is not None else _config("FUNES_PARALLEL_API_KEY")
        self.parallel_url = parallel_url
        self._session: str | None = None
        self.searches = 0
        self.tavily_calls = 0
        self.rate_limited = 0
        self.search_errors = 0          # Parallel answered with an error (any status but 429-then-recovered)
        self.error_log: list[dict] = []  # {"status", "text"} of each, the evidence kept in the result JSON
        self._last = 0.0
        self.sleep = time.sleep
        self.fetches = 0
        self.calls: list[dict] = []     # [{"backend": "parallel"|"tavily", "fallback_reason": ...}]
        # What the engines actually sent back, before any formatting or
        # truncation: the web cells can be re-scored only if this is kept.
        self.payloads: list[dict] = []
        self._attempts: list[dict] = []

    @property
    def available(self) -> bool:
        return self.backend == "parallel" or bool(self.key)

    # -- Parallel (MCP, streamable HTTP) ------------------------------------
    def _rpc(self, body: dict, session: str | None):
        headers = {"Accept": "application/json, text/event-stream"}
        if session:
            headers["Mcp-Session-Id"] = session
        if self.parallel_key:
            headers["Authorization"] = f"Bearer {self.parallel_key}"
        return self.post(self.parallel_url, body, headers)

    def _open(self) -> str:
        text, hdr = self._rpc({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-03-26", "capabilities": {},
            "clientInfo": {"name": "funes-ko-bench", "version": "0"}}}, None)
        _mcp_json(text)
        sid = {k.lower(): v for k, v in hdr.items()}.get("mcp-session-id")
        self._rpc({"jsonrpc": "2.0", "method": "notifications/initialized"}, sid)
        return sid or ""

    BACKOFF = (5, 15, 45, 120)      # seconds to wait out a 429 before giving Parallel up
    MIN_GAP = 0.0                   # grows after each 429: the pace Parallel tolerates

    def _parallel(self, query: str) -> str:
        for wait in (0, *self.BACKOFF):
            if wait:
                self.sleep(wait)
            gap = self.MIN_GAP - (time.monotonic() - self._last)
            if gap > 0:
                self.sleep(gap)
            self._last = time.monotonic()
            try:
                return self._parallel_once(query)
            except urllib.error.HTTPError as e:
                if e.code != 429:
                    raise
                self.MIN_GAP = min(self.MIN_GAP + 1.0, 10.0)
                self.rate_limited += 1
        raise SearchBlocked(f"parallel returned 429 on {1 + len(self.BACKOFF)} attempts over the backoff")

    def _parallel_once(self, query: str) -> str:
        for attempt in (0, 1):             # a stale session gets one fresh start
            try:
                if self._session is None:
                    self._session = self._open()
                text, _ = self._rpc({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
                    "name": "web_search",
                    "arguments": {"objective": query, "search_queries": [query]}}},
                    self._session or None)
                self._attempts.append({"backend": "parallel", "raw": text})
                msg = _mcp_json(text)
                break
            except Exception as e:
                self._session = None
                self._attempts.append({"backend": "parallel", "error": f"{type(e).__name__}: {e}"[:300]})
                if attempt or (isinstance(e, urllib.error.HTTPError) and e.code == 429):
                    raise
        res = msg.get("result", {})
        body = "".join(c.get("text", "") for c in res.get("content", []) if c.get("type") == "text")
        if res.get("isError") or "error" in msg:
            raise RuntimeError(f"parallel error: {body or msg.get('error')}")
        hits = json.loads(body).get("results", [])
        lines = []
        for h in hits[:5]:
            title = " ".join((h.get("title") or "").split())
            exc = " … ".join(" ".join(e.split()) for e in (h.get("excerpts") or []) if isinstance(e, str))
            if title and h.get("url"):
                lines.append(f"{title} | {h['url']} | {exc[:600]}")
        return "\n".join(lines) or "no results"

    # -- Tavily ---------------------------------------------------------------
    TAVILY_CAP = 800        # of the 1,000 free a month: never spend past this

    def _tavily(self, query: str) -> str:
        if not self.key:
            raise RuntimeError("no Tavily key")
        if self.tavily_calls >= self.TAVILY_CAP:
            raise RuntimeError(f"Tavily cap of {self.TAVILY_CAP} calls reached")
        self.tavily_calls += 1
        raw = self.transport("https://api.tavily.com/search",
                             {"api_key": self.key, "query": query, "max_results": 5,
                              "search_depth": "basic", "include_answer": False})
        self._attempts.append({"backend": "tavily", "raw": raw})
        results = json.loads(raw).get("results", [])
        return "\n".join(f"{r.get('title','')} | {r.get('url','')} | "
                         f"{(r.get('content') or '')[:400]}" for r in results) or "no results"

    def search(self, query: str) -> str:
        self._attempts = []
        try:
            return self._search(query)
        finally:
            self.payloads.append({"kind": "search", "query": query, "attempts": self._attempts,
                                  "backend": self.calls[-1]["backend"] if self.calls else None})

    def _search(self, query: str) -> str:
        self.searches += 1
        if self.backend == "parallel":
            try:
                out = self._parallel(query)
                self.calls.append({"backend": "parallel"})
                return out
            except Exception as e:
                reason = f"{type(e).__name__}: {str(e)[:120]}"
                if not isinstance(e, SearchBlocked):
                    self.search_errors += 1
                    self.error_log.append({"status": error_status(e), "text": f"{type(e).__name__}: {e}"[:400]})
                if not self.key:
                    self.calls.append({"backend": "none", "fallback_reason": reason})
                    if isinstance(e, SearchStop):
                        raise
                    raise SearchFailed(f"parallel failed and no Tavily key: {reason}", error_status(e)) from e
                out = self._tavily(query)
                self.calls.append({"backend": "tavily", "fallback_reason": reason})
                return out
        out = self._tavily(query)
        self.calls.append({"backend": "tavily"})
        return out

    def fetch(self, url: str) -> str:
        if not re.match(r"^https?://", url or ""):
            return "error: only http(s) URLs"
        self.fetches += 1
        raw = self.transport(url, None)
        self.payloads.append({"kind": "fetch", "url": url, "raw": raw})
        text = re.sub(r"(?is)<(script|style).*?</\1>", " ", raw)
        text = html.unescape(re.sub(r"<[^>]+>", " ", text))
        return " ".join(text.split())[:self.max_chars]


class Notes:
    """The `web+cache` memory: what the model chose to remember, searchable.
    Facts it stores are tainted by construction (they came from the web)."""

    def __init__(self):
        self.store = FactStore()
        self.count = 0
        self.served = 0          # recalls that returned a note (a tainted source)

    def remember(self, fact: str) -> str:
        self.store.add(Fact(subj=fact[:500], prop="note", obj=""))
        self.count += 1
        return "saved"

    def writeback(self, question: str, answer: str, url: str) -> None:
        """The harness writes the note, not the model: "Q … A … (per url)"."""
        self.store.add(Fact(subj=f"Q: {question} A: {answer} (per {url})", prop="note", obj=""))
        self.count += 1

    def recall(self, query: str) -> str:
        hits = self.store.search(query, k=3)
        self.served += bool(hits)
        return "\n".join(f.subj for f in hits) or "nothing remembered about that"
