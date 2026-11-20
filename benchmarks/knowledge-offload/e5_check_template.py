#!/usr/bin/env python3
"""E5 gates 9a/9b (Opus review of 982de6d): the chat template and the loss mask, checked before any training.

  python3 e5_check_template.py --rows lookup_L1_n1000.jsonl --model-dir data/e5/qwen3.5-2B-bf16 \
      [--server http://127.0.0.1:8199] [--n 20] [--out report.json]

For `--n` rows (the first ones, deterministic) and for every assistant message:
  * prefix property: the text rendered up to that message *with* the generation prompt is a prefix of the text
    rendered through it, so what is trained is the suffix and nothing the model would have been given;
  * the trained span (suffix up to and including <|im_end|>, the newline after it is not trained) decoded from the
    masked-in tokens equals the span text exactly (9b), and masked-in tokens cover only that span;
  * with `--server` (a llama-server loaded with a Qwen3.5 GGUF): llama-server's /apply-template output for the same
    messages equals ours (9a, text level), and /tokenize of our text gives the same token ids as `tokenizers` (9a,
    token level). Without `--server` those two are reported as NOT RUN, never as passed.
Reads the chat template and tokenizer.json of the model directory only; renders in a Jinja sandbox. No model, no GPU.
"""
import argparse
import json
import sys
import urllib.request
from pathlib import Path

from jinja2.sandbox import ImmutableSandboxedEnvironment
from tokenizers import Tokenizer

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ko import e5_data as E  # noqa: E402

IM_END = "<|im_end|>"


class _Raise(Exception):
    pass


def _raise(msg):
    raise _Raise(msg)


def load_template(model_dir: Path):
    env = ImmutableSandboxedEnvironment(trim_blocks=True, lstrip_blocks=True)
    env.globals["raise_exception"] = _raise
    env.filters["tojson"] = lambda v, **kw: json.dumps(v, ensure_ascii=False)
    return env.from_string((model_dir / "chat_template.jinja").read_text(encoding="utf-8"))


def render(tpl, messages, tools, enable_thinking: bool, add_generation_prompt: bool) -> str:
    return tpl.render(messages=messages, tools=tools, add_generation_prompt=add_generation_prompt,
                      enable_thinking=enable_thinking)


def trained_spans(tpl, row: dict) -> list[dict]:
    """For each trained message: the char span (start, end) in the full render, and its text."""
    msgs = E.template_messages(row)
    tools = row.get("tools")
    et = bool(row["chat_template_kwargs"].get("enable_thinking", False))
    full = render(tpl, msgs, tools, et, False)
    out = []
    for m in row["mask"]:
        if not m["train"]:
            continue
        i = m["index"]
        head = render(tpl, msgs[:i], tools, et, True)       # what the model is given before this turn
        upto = render(tpl, msgs[:i + 1], tools, et, False)  # through this turn
        if not upto.startswith(head):
            raise _Raise(f"{row['id']} message {i}: generation-prompt render is not a prefix of the render through the message")
        if not full.startswith(upto):
            raise _Raise(f"{row['id']} message {i}: render through the message is not a prefix of the full render")
        end = upto.rfind(IM_END)
        if end < len(head):
            raise _Raise(f"{row['id']} message {i}: no {IM_END} after the generation prompt")
        end += len(IM_END)
        out.append({"index": i, "what": m["what"], "start": len(head), "end": end, "text": upto[len(head):end]})
    return out, full


def masked_tokens(tok: Tokenizer, full: str, spans: list[dict]) -> dict:
    enc = tok.encode(full, add_special_tokens=False)
    train = [any(s["start"] <= a and b <= s["end"] for s in spans) and b > a for (a, b) in enc.offsets]
    straddle = [t for t, (a, b) in zip(enc.tokens, enc.offsets)
                if any((a < s["start"] < b) or (a < s["end"] < b) for s in spans)]
    decoded = []
    for s in spans:
        ids = [i for i, (a, b), t in zip(enc.ids, enc.offsets, train) if t and s["start"] <= a and b <= s["end"]]
        decoded.append(tok.decode(ids, skip_special_tokens=False))
    return {"ids": enc.ids, "train": train, "decoded": decoded, "straddling_tokens": straddle}


def _post(url: str, body: dict) -> dict:
    req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read())


def server_checks(server: str, tpl, row: dict, full: str, ids: list[int]) -> dict:
    """9a against a running llama-server: its own rendering and tokenisation of the same conversation.
    /apply-template treats a trailing assistant message as a prefill (no closing <|im_end|>), so each trained turn
    is compared by what the model is *given*: the render of the messages before it plus the generation prompt;
    the full conversation is compared up to the final message's content."""
    msgs, tools = E.template_messages(row), row.get("tools")
    et = bool(row["chat_template_kwargs"].get("enable_thinking", False))
    kw = row["chat_template_kwargs"]

    def srv(m):
        return _post(server + "/apply-template", {"messages": m, "tools": tools, "chat_template_kwargs": kw}).get("prompt")

    heads = {}
    for m in row["mask"]:
        if m["train"]:
            i = m["index"]
            ours, theirs = render(tpl, msgs[:i], tools, et, True), srv(msgs[:i])
            heads[i] = {"equal": ours == theirs, "ours_tail": None if ours == theirs else ours[-120:],
                        "theirs_tail": None if ours == theirs else (theirs or "")[-120:]}
    got = srv(msgs)
    ours_full = full[:-len(IM_END + "\n")] if full.endswith(IM_END + "\n") else full
    tk = _post(server + "/tokenize", {"content": full, "add_special": False, "parse_special": True}).get("tokens")
    return {"heads_equal": all(h["equal"] for h in heads.values()), "heads": heads,
            "full_equal_to_final_content": got == ours_full, "tokens_equal": tk == ids,
            "n_tokens_ours": len(ids), "n_tokens_server": len(tk or [])}


def check_row(tpl, tok, row: dict, server: str | None) -> dict:
    spans, full = trained_spans(tpl, row)
    mt = masked_tokens(tok, full, spans)
    res = {"id": row["id"], "n_tokens": len(mt["ids"]), "n_trained_tokens": sum(mt["train"]),
           "spans": [], "straddling_tokens": mt["straddling_tokens"]}
    for s, dec in zip(spans, mt["decoded"]):
        res["spans"].append({"what": s["what"], "text": s["text"], "decoded_equal": dec == s["text"],
                             "decoded": None if dec == s["text"] else dec})
    if server:
        res["server"] = server_checks(server, tpl, row, full, mt["ids"])
    return res


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", required=True, nargs="+", help="JSONL files of builder rows (L1, L0, anchors)")
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--n", type=int, default=20, help="rows per file")
    ap.add_argument("--server", default=None)
    ap.add_argument("--out", default=None)
    a = ap.parse_args(argv)
    md = Path(a.model_dir)
    tpl, tok = load_template(md), Tokenizer.from_file(str(md / "tokenizer.json"))
    report, bad = [], 0
    for f in a.rows:
        rows = [json.loads(l) for l in Path(f).read_text(encoding="utf-8").splitlines() if l.strip()][:a.n]
        for row in rows:
            r = check_row(tpl, tok, row, a.server)
            r["file"] = Path(f).name
            ok = (all(s["decoded_equal"] for s in r["spans"]) and not r["straddling_tokens"]
                  and (not a.server or (r["server"]["heads_equal"] and r["server"]["full_equal_to_final_content"] and r["server"]["tokens_equal"])))
            r["ok"] = ok
            bad += not ok
            report.append(r)
    print(f"rows checked: {len(report)}; failing: {bad}; server checks: {'RUN' if a.server else 'NOT RUN'}")
    for r in report[:2]:
        print(f"--- {r['file']} {r['id']}: {r['n_trained_tokens']}/{r['n_tokens']} tokens trained")
        for s in r["spans"]:
            print(f"  [{s['what']}] {s['text']!r}")
    if a.out:
        Path(a.out).write_text(json.dumps(report, indent=1, ensure_ascii=False))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
