"""llama-server client. Inference only ever goes to yoda's endpoints."""
import json
import re
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field

# Standing rule: no model inference on the laptop. Refuse a loopback endpoint
# unless the person says it is a tunnel to yoda (--allow-local).
_LOCAL = re.compile(r"^https?://(localhost|127\.|\[::1\])", re.I)


class LLMError(RuntimeError):
    pass


@dataclass
class Usage:
    """Cost of one answer, from llama-server's own `timings` block."""
    prompt_tokens: int = 0
    completion_tokens: int = 0
    prompt_ms: float = 0.0
    predicted_ms: float = 0.0
    wall_ms: float = 0.0
    calls: int = 0

    def add(self, other: "Usage") -> None:
        for k in self.__dataclass_fields__:
            setattr(self, k, getattr(self, k) + getattr(other, k))

    def as_dict(self) -> dict:
        return {k: round(getattr(self, k), 2) for k in self.__dataclass_fields__}


@dataclass
class Reply:
    text: str
    tool_calls: list = field(default_factory=list)   # [{id, name, arguments(dict)}]
    usage: Usage = field(default_factory=Usage)
    raw_message: dict = field(default_factory=dict)
    finish_reason: str = ""


class Client:
    def __init__(self, base_url: str, allow_local: bool = False, timeout: float = 300):
        if _LOCAL.match(base_url) and not allow_local:
            raise LLMError(f"{base_url} is local; inference runs on yoda only "
                           "(pass --allow-local if this is a tunnel to yoda)")
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def _json(self, path: str, body: dict | None = None):
        req = urllib.request.Request(
            self.base_url + path, method="POST" if body is not None else "GET",
            data=json.dumps(body).encode() if body is not None else None,
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return json.load(r)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
            raise LLMError(f"{path}: {e}") from e

    def server_info(self) -> dict:
        """What is actually being served: model file, quant, context. Recorded
        in every result so a swap mid-run cannot go unnoticed."""
        models = self._json("/v1/models")
        entry = (models.get("models") or models.get("data") or [{}])[0]
        name = entry.get("name") or entry.get("id") or ""
        m = re.search(r"(IQ\d_\w+|Q\d_[A-Z0-9_]+|Q\d_\d|BF16|F16)", name, re.I)
        props = {}
        try:
            props = self._json("/props")
        except LLMError:
            pass
        return {"endpoint": self.base_url, "model_file": name,
                "quant": m.group(1).upper() if m else "unknown",
                "n_ctx": (props.get("default_generation_settings") or {}).get("n_ctx"),
                "build": props.get("build_info")}

    def chat(self, messages, tools=None, tool_choice=None, seed=0,
             max_tokens=256, temperature=0.0) -> Reply:
        body = {"messages": messages, "temperature": temperature, "seed": seed,
                "max_tokens": max_tokens, "stream": False,
                # Memory note: thinking swallows tool calls; the plan's models
                # are served --jinja, so switch it off per request too.
                "chat_template_kwargs": {"enable_thinking": False}}
        if tools:
            body["tools"] = tools
            body["tool_choice"] = tool_choice or "auto"
        t0 = time.monotonic()
        resp = self._json("/v1/chat/completions", body)
        wall = (time.monotonic() - t0) * 1000
        try:
            msg = resp["choices"][0]["message"]
        except (KeyError, IndexError) as e:
            raise LLMError(f"malformed completion: {str(resp)[:200]}") from e
        tm = resp.get("timings") or {}
        us = resp.get("usage") or {}
        usage = Usage(
            prompt_tokens=tm.get("prompt_n", us.get("prompt_tokens", 0)),
            completion_tokens=tm.get("predicted_n", us.get("completion_tokens", 0)),
            prompt_ms=tm.get("prompt_ms", 0.0), predicted_ms=tm.get("predicted_ms", 0.0),
            wall_ms=wall, calls=1)
        calls = []
        for tc in msg.get("tool_calls") or []:
            fn = tc.get("function", {})
            try:
                args = json.loads(fn.get("arguments") or "{}")
            except json.JSONDecodeError:
                args = {"_unparsed": fn.get("arguments")}
            calls.append({"id": tc.get("id", ""), "name": fn.get("name", ""),
                          "arguments": args if isinstance(args, dict) else {"_value": args}})
        return Reply(text=(msg.get("content") or "").strip(), tool_calls=calls,
                     usage=usage, raw_message=msg,
                     finish_reason=resp["choices"][0].get("finish_reason") or "")
