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


# A scenario keyword anywhere in the user side of the conversation, not just in
# the newest turn: the agent loop injects its own user turns (nudges), so the
# original request stops being `last_user` as soon as one lands.
def mentions(messages, keyword):
    return any(keyword in (m.get("content") or "")
               for m in messages if m.get("role") == "user")


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

        # Completion-contract scenarios (AgentConfig::require_tools). Both
        # reproduce the real failure this feature exists for: the model answers
        # in prose at a step boundary instead of making the next tool call.
        # "contract-comply" gives in once nudged; "contract-refuse" never does,
        # so the run must end in an explicit failure rather than a false success.
        if "contract-" in last_user or any(
                "contract-" in m.get("content", "")
                for m in messages if m.get("role") == "user"):
            nudged = any("Emit the next tool call now" in m.get("content", "")
                         for m in messages if m.get("role") == "user")
            refusing = any("contract-refuse" in m.get("content", "")
                           for m in messages if m.get("role") == "user")
            if nudged and not refusing and not has_tool_result:
                tool_call = {"id": "call_write", "type": "function",
                             "function": {"name": "write_file",
                                          "arguments": json.dumps({
                                              "path": "contract_proof.txt",
                                              "content": "written under protest"})}}
                if stream:
                    self._stream_tool_call(tool_call)
                else:
                    self._json(200, {"choices": [{"message": {
                        "role": "assistant", "content": None,
                        "tool_calls": [tool_call]}}],
                        "usage": {"prompt_tokens": 1, "completion_tokens": 1}})
                return
            if not has_tool_result:
                text = "MOCK-PREMATURE-ANSWER: I have written the file. All done!"
                if stream:
                    self._stream_text(text)
                else:
                    self._json(200, {"choices": [{"message": {
                        "role": "assistant", "content": text}}],
                        "usage": {"prompt_tokens": 1, "completion_tokens": 1}})
                return

        # Result store (core/result_store.h): a large tool result arrives as a
        # preview carrying a result_id instead of the content itself, and the
        # model reaches the rest through read_result. Modelled end to end here
        # because the interesting part is the handoff, not any one piece.
        if mentions(messages, "big-result"):
            stored_id, window = None, None
            for m in messages:
                if m.get("role") != "tool":
                    continue
                if m.get("name") == "read_result":
                    window = m.get("content") or ""
                    continue
                try:
                    payload = json.loads(m.get("content") or "")
                except ValueError:
                    continue
                if isinstance(payload, dict) and "result_id" in payload:
                    stored_id = payload["result_id"]

            if stored_id is None and window is None:
                self._reply_tool({"id": "call_read", "type": "function",
                                  "function": {"name": "read_file",
                                               "arguments": json.dumps({"path": "big.txt"})}},
                                 stream)
            elif window is None:
                self._reply_tool({"id": "call_deref", "type": "function",
                                  "function": {"name": "read_result",
                                               "arguments": json.dumps({"id": stored_id,
                                                                        "offset": 0,
                                                                        "limit": 64})}},
                                 stream)
            else:
                self._reply_text("MOCK-REPLY dereferenced id=%s window=%s"
                                 % (stored_id, window[:20].replace("\n", " ")), stream)
            return

        # Regression: a model that keeps re-issuing the same call on a big
        # result until the loop detector gives up. Every bailout answer is
        # built from last_tool_result, and those answers are read by a
        # *person* — so they must carry the result's own text, not the preview
        # envelope the model was shown. (Shipped broken in 2.0: the user got
        # {"result_id":1,"head":…} as the reply.)
        if mentions(messages, "big-loop"):
            self._reply_tool({"id": "call_big", "type": "function",
                              "function": {"name": "read_file",
                                           "arguments": json.dumps({"path": "big.txt"})}},
                             stream)
            return

        # The reported failure, end to end: a sub-agent gives up, and the
        # answer it hands back must not read as content to its caller. The
        # parent is identified by the absence of the child's task text — the
        # sub-agent runs with "sub-gives-up" as its user message.
        # Per-tool ceiling (core/tool_budget.h). A model that would search
        # forever: it keeps asking for the same tool and must be refused, then
        # conclude. Identical arguments on purpose — the refusal has to
        # outrank the identical-args loop detector, or a budget below 3 could
        # never fire and the run would die instead of concluding.
        if mentions(messages, "budget-burn"):
            refused = any("will not run" in (m.get("content") or "")
                          for m in messages if m.get("role") == "tool")
            if refused:
                self._reply_text("MOCK-CONCLUDED from what I had", stream)
            else:
                self._reply_tool({"id": "call_budget", "type": "function",
                                  "function": {"name": "read_file",
                                               "arguments": json.dumps({"path": "small.txt"})}},
                                 stream)
            return

        # The same ceiling, against a model that ignores the refusal. The
        # cooperative case above concludes the moment it is told to; the real
        # 9B on 2026-07-31 did not — it re-issued the refused web_search seven
        # times in a row until max_steps ended researcher with no answer, and
        # the whole newsletter pipeline collapsed behind it. Asking is
        # therefore not enough: the framework has to take the option away. So
        # this mock never concludes voluntarily, and answers with text only
        # when tools have actually been withheld.
        if mentions(messages, "budget-stubborn"):
            # Withholding shows up as an absent tool schema, not as a
            # tool_choice field — a model is told by what it is shown.
            if not req.get("tools"):
                self._reply_text("MOCK-FORCED-CONCLUSION", stream)
            else:
                self._reply_tool({"id": "call_stubborn", "type": "function",
                                  "function": {"name": "read_file",
                                               "arguments": json.dumps({"path": "small.txt"})}},
                                 stream)
            return

        # The same shape again, but what runs out is steps rather than a
        # per-tool budget. This mock calls read_file forever if allowed to, so
        # the run ends with an answer only if the framework withholds tools on
        # the final step. An uncapped tool produces no refusal, so the budget
        # mechanism above never fires — which is how researcher spent all 20 of
        # its steps on read_result and returned nothing on 2026-07-31.
        if mentions(messages, "step-hog"):
            if not req.get("tools"):
                self._reply_text("MOCK-LAST-STEP-ANSWER", stream)
            else:
                self._reply_tool({"id": "call_hog", "type": "function",
                                  "function": {"name": "read_file",
                                               "arguments": json.dumps({"path": "small.txt"})}},
                                 stream)
            return

        # Withholding tools is defeated if the model can write a call as prose
        # and have it recovered. A server asked for tool_choice "none" emits no
        # native call, so the 9B wrote <function=web_search> into content
        # instead and recover_tool_calls_from_content turned it back into a
        # call — on 2026-07-31 that undid both the budget refusal and the
        # last-step reservation, and researcher exhausted its steps anyway.
        #
        # This mock is the worst case: it emits the XML no matter what it is
        # told. Withholding must survive a model that never cooperates, so the
        # run has to end without executing that call.
        if mentions(messages, "xml-hog"):
            self._reply_text(
                "<tool_call><function=read_file>"
                "<parameter=path>small.txt</parameter>"
                "</function></tool_call>", stream)
            return

        # The cooperative counterpart: it writes the XML only while tools are
        # in the prompt, and answers once they aren't. Setting tool_choice is
        # not enough on its own — a model that can still see the tool schema
        # keeps reaching for it — so the withheld turn must send no tools at
        # all, the way the empty-completion retry always has.
        if mentions(messages, "xml-yielder"):
            if not req.get("tools"):
                self._reply_text("MOCK-YIELDED-ANSWER", stream)
            else:
                self._reply_text(
                    "<tool_call><function=read_file>"
                    "<parameter=path>small.txt</parameter>"
                    "</function></tool_call>", stream)
            return

        # The same dereference, but asking for a full window rather than a
        # 64-byte peek — which is what a real model does when it wants the
        # content. result_window appends a "N bytes remain" note after the
        # window, so a full one lands just over kInlineLimit and used to be
        # stored in turn: reading a stored result produced another stored
        # result, and the model got a fresh result_id instead of the text every
        # time. That is what burned researcher's 20 steps on 2026-07-31.
        if mentions(messages, "deref-full"):
            stored_id, window = None, None
            for m in messages:
                if m.get("role") != "tool":
                    continue
                if m.get("name") == "read_result":
                    window = m.get("content") or ""
                    continue
                try:
                    payload = json.loads(m.get("content") or "")
                except ValueError:
                    continue
                if isinstance(payload, dict) and "result_id" in payload:
                    stored_id = payload["result_id"]

            if stored_id is None and window is None:
                self._reply_tool({"id": "call_read", "type": "function",
                                  "function": {"name": "read_file",
                                               "arguments": json.dumps({"path": "big.txt"})}},
                                 stream)
            elif window is None:
                self._reply_tool({"id": "call_deref", "type": "function",
                                  "function": {"name": "read_result",
                                               "arguments": json.dumps({"id": stored_id,
                                                                        "offset": 0,
                                                                        "limit": 2048})}},
                                 stream)
            else:
                # Echo enough to prove the window itself came back, not another
                # preview envelope pointing at yet another stored result.
                self._reply_text("MOCK-DEREF-FULL got=%s" % window[:12].replace("\n", " "),
                                 stream)
            return

        # Withholding has to be announced, not just performed. This model does
        # what the 9B did on 2026-07-31: its transcript is full of tool calls,
        # so it keeps writing them as prose even on a turn that ships no tool
        # schema at all. It only stops once the loop says so in words.
        if mentions(messages, "needs-telling"):
            if mentions(messages, "No tools are available for this turn"):
                self._reply_text("MOCK-TOLD-SO-ANSWERED", stream)
            else:
                self._reply_text(
                    "<tool_call><function=read_file>"
                    "<parameter=path>small.txt</parameter>"
                    "</function></tool_call>", stream)
            return

        # The child runs as its own conversation, with the task as its only
        # user message — so it is keyed on the task text, not on the parent's.
        if mentions(messages, "sub-gives-up"):
            self._reply_text("", stream)              # the child: no answer
            return

        if mentions(messages, "delegate-boom"):
            if not has_tool_result:
                self._reply_tool({"id": "call_sub", "type": "function",
                                  "function": {"name": "delegate_to_agent",
                                               "arguments": json.dumps({
                                                   "agent": "researcher",
                                                   "task": "sub-gives-up"})}},
                                 stream)
            else:
                self._reply_text("MOCK-PARENT-SAW: " + (
                    "error" if any("FAILED" in (m.get("content") or "")
                                   for m in messages if m.get("role") == "tool")
                    else "content"), stream)
            return

        # The empty-completion salvage retry (agent.cpp): when a model returns
        # no content right after a tool round trip, the loop makes one more
        # call with tools disabled to shake the answer out. Here that retry
        # itself fails. It is a *salvage* — the tool result is already in hand
        # — so its failure must not become the user's answer.
        if mentions(messages, "retry-boom"):
            if not has_tool_result:
                self._reply_tool({"id": "call_small", "type": "function",
                                  "function": {"name": "read_file",
                                               "arguments": json.dumps({"path": "small.txt"})}},
                                 stream)
            elif "tools" not in req:
                # No tools in the body ⇒ this is the salvage retry, not a
                # normal step. build_openai_body omits the key entirely.
                self._json(500, {"error": "salvage retry is down"})
            else:
                self._reply_text("", stream)
            return

        # Answer schema (core/answer_schema.h). Same shape as the contract
        # scenarios above: "schema-comply" gets it right once told what was
        # wrong, "schema-refuse" never does and must fail loudly.
        if mentions(messages, "schema-"):
            schema_nudged = any("does not match the required format" in (m.get("content") or "")
                                for m in messages if m.get("role") == "user")
            if schema_nudged and not mentions(messages, "schema-refuse"):
                self._reply_text(
                    "Sure — here it is:\n```json\n"
                    "{\"summary\": \"the mock answer\", "
                    "\"sources\": [\"https://example.com\"]}\n```", stream)
            else:
                self._reply_text("MOCK-PROSE-ANSWER: I looked it all up, trust me.", stream)
            return

        text = "MOCK-REPLY"
        if has_tool_result:
            text += " with-tool-result"
        if "Relevant memories" in system_text:
            text += " with-memories"

        self._reply_text(text, stream)

    # ── reply helpers ────────────────────────────────────────────────────────

    def _reply_text(self, text, stream):
        if stream:
            self._stream_text(text)
        else:
            self._json(200, {"choices": [{"message": {
                "role": "assistant", "content": text}}],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1}})

    def _reply_tool(self, tool_call, stream):
        if stream:
            self._stream_tool_call(tool_call)
        else:
            self._json(200, {"choices": [{"message": {
                "role": "assistant", "content": None,
                "tool_calls": [tool_call]}}],
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
