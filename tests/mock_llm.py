#!/usr/bin/env python3
"""Mock OpenAI-compatible LLM server for Funes integration tests.

Behavior:
- POST /v1/chat/completions (streaming and non-streaming)
- If the last user message contains "use-tool" and no tool result is in the
  history yet, responds with a `recall` tool call — exercising the agent loop.
- Otherwise answers with a fixed text that echoes whether a tool result or
  injected memories were seen, so the test can assert on the pipeline.
- POST /v1/embeddings returns a deterministic 8-dim vector.
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def fake_embedding(text):
    vec = [0.0] * 8
    for i, ch in enumerate(text.encode()):
        vec[i % 8] += ch / 255.0
    norm = sum(x * x for x in vec) ** 0.5 or 1.0
    return [x / norm for x in vec]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(length) or b"{}")

        if self.path == "/v1/embeddings":
            self._json(200, {"data": [{"embedding": fake_embedding(req.get("input", ""))}]})
            return

        if self.path != "/v1/chat/completions":
            self._json(404, {"error": "unknown path"})
            return

        messages = req.get("messages", [])
        stream = req.get("stream", False)
        last_user = next((m["content"] for m in reversed(messages)
                          if m.get("role") == "user"), "")
        has_tool_result = any(m.get("role") == "tool" for m in messages)
        system_text = next((m["content"] for m in messages
                            if m.get("role") == "system"), "")

        if "use-tool" in last_user and not has_tool_result:
            tool_call = {"id": "call_1", "type": "function",
                         "function": {"name": "recall",
                                      "arguments": json.dumps({"query": "test"})}}
            if stream:
                self._stream_tool_call(tool_call)
            else:
                self._json(200, {"choices": [{"message": {
                    "role": "assistant", "content": None,
                    "tool_calls": [tool_call]}}],
                    "usage": {"prompt_tokens": 1, "completion_tokens": 1}})
            return

        if "delegate-now" in last_user and not has_tool_result:
            tool_call = {"id": "call_delegate", "type": "function",
                         "function": {"name": "delegate_to_agent",
                                      "arguments": json.dumps({
                                          "agent": "researcher",
                                          "task": "look something up"})}}
            if stream:
                self._stream_tool_call(tool_call)
            else:
                self._json(200, {"choices": [{"message": {
                    "role": "assistant", "content": None,
                    "tool_calls": [tool_call]}}],
                    "usage": {"prompt_tokens": 1, "completion_tokens": 1}})
            return

        text = "MOCK-REPLY"
        if has_tool_result:
            text += " with-tool-result"
        if "Relevant memories" in system_text:
            text += " with-memories"

        if stream:
            self._stream_text(text)
        else:
            self._json(200, {"choices": [{"message": {
                "role": "assistant", "content": text}}],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1}})

    def _sse_headers(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

    def _sse(self, obj):
        self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")

    def _stream_text(self, text):
        self._sse_headers()
        for i in range(0, len(text), 4):
            self._sse({"choices": [{"delta": {"content": text[i:i + 4]}}]})
        self._sse({"choices": [{"delta": {}, "finish_reason": "stop"}]})
        self.wfile.write(b"data: [DONE]\n\n")

    def _stream_tool_call(self, tc):
        self._sse_headers()
        args = tc["function"]["arguments"]
        self._sse({"choices": [{"delta": {"tool_calls": [
            {"index": 0, "id": tc["id"],
             "function": {"name": tc["function"]["name"], "arguments": ""}}]}}]})
        # arguments split across two chunks, as real APIs do
        half = len(args) // 2
        for part in (args[:half], args[half:]):
            self._sse({"choices": [{"delta": {"tool_calls": [
                {"index": 0, "function": {"arguments": part}}]}}]})
        self._sse({"choices": [{"delta": {}, "finish_reason": "tool_calls"}]})
        self.wfile.write(b"data: [DONE]\n\n")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
