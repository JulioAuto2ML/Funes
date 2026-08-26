#!/usr/bin/env python3
"""
funes_bench.py -- tool-calling benchmark for whatever model is wired into Funes.

Point this at a running `funes` instance (any FUNES_LLM_URL/PROVIDER/MODEL) and
it drives the real /api/chat SSE endpoint through a fixed suite of prompts,
each designed to require (or forbid) a specific tool call, and scores whether
the connected model actually behaves the way Funes' harness expects.

This is deliberately NOT a generic tool-calling benchmark like BFCL -- it
exercises Funes' *actual* agents, *actual* tool schemas, and *actual* safety
harness (tool budgets, delegation, memory). The question it answers is "if I
point Funes at model X, does it work well enough here", not "how does model X
rank on an academic leaderboard".

Stdlib only, no external dependencies (matches the rest of scripts/).

Usage:
    # smoke test against a local instance, default agent
    python3 funes_bench.py

    # tag the run so reports are comparable across models later
    python3 funes_bench.py --tag qwen3-8b-q4 --out results/

    # just list what would run
    python3 funes_bench.py --list

    # skip anything that needs web_search/web_fetch (no Tavily key configured)
    python3 funes_bench.py --skip-web

    # run one case while iterating
    python3 funes_bench.py --case file_roundtrip

    # compare saved reports side by side
    python3 funes_bench.py --compare results/*.json
"""

from __future__ import annotations

import argparse
import glob
import json
import sys
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Optional


# ──────────────────────────────────────────────────────────────────────────
# Wire format (matches src/server/api.cpp / src/core/agent.cpp as of the
# harness described in docs/TECHNICAL.md -- re-check that doc if this script
# starts getting empty tool_call names or a 400 on every request, the wire
# format is the first thing to have drifted).
#
#   POST /api/chat  {"message": str, "session": str, "agent": str}
#     session must match [A-Za-z0-9_-]{1,64}
#   -> SSE stream, one event per "event: TYPE\ndata: JSON\n\n" block:
#       tool_call   {"name": str, "args": obj}
#       tool_result {"name": str, "preview": str, "error": str}
#       done        {"text": str}
#       error       {"message": str}
#       usage       {"used": int, "limit": int, "estimated": bool}
#     (memories / delta / result_stored / contract_nudge / schema_nudge /
#     context_compressed also occur but aren't needed for scoring here)
#
#   A run that hit a harness failure (loop kill, max_steps, empty answer,
#   unsatisfied completion contract) returns its `done` text prefixed with
#   "FAILED -- " (run_outcome.h's kFailureMarker) -- that always fails a case
#   regardless of which tools were called.
# ──────────────────────────────────────────────────────────────────────────

FAILURE_MARKER = "FAILED --"


# ── SSE client ───────────────────────────────────────────────────────────

@dataclass
class ChatResult:
    tool_calls: list[dict] = field(default_factory=list)   # [{"name":..., "args":...}]
    tool_results: list[dict] = field(default_factory=list)
    final_text: str = ""
    error: Optional[str] = None
    usage: Optional[dict] = None
    first_event_s: Optional[float] = None
    total_s: float = 0.0
    timed_out: bool = False


def chat(base_url: str, agent: str, session: str, message: str, timeout: float) -> ChatResult:
    """POST one message to /api/chat and consume the SSE stream to completion."""
    body = json.dumps({"message": message, "session": session, "agent": agent}).encode("utf-8")
    req = urllib.request.Request(
        base_url.rstrip("/") + "/api/chat",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    result = ChatResult()
    start = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            event_type: Optional[str] = None
            for raw_line in resp:
                line = raw_line.decode("utf-8", errors="replace").rstrip("\n").rstrip("\r")
                if result.first_event_s is None and line:
                    result.first_event_s = time.monotonic() - start
                if line.startswith("event: "):
                    event_type = line[len("event: "):]
                elif line.startswith("data: "):
                    data_raw = line[len("data: "):]
                    try:
                        data = json.loads(data_raw)
                    except json.JSONDecodeError:
                        data = {}
                    if event_type == "tool_call":
                        result.tool_calls.append({"name": data.get("name", ""), "args": data.get("args", {})})
                    elif event_type == "tool_result":
                        result.tool_results.append(data)
                    elif event_type == "done":
                        result.final_text = data.get("text", "")
                    elif event_type == "error":
                        result.error = data.get("message", "")
                    elif event_type == "usage":
                        result.usage = data
                    event_type = None
                elif line == "":
                    event_type = None
    except urllib.error.URLError as e:
        result.error = f"connection error: {e}"
    except TimeoutError:
        result.timed_out = True
        result.error = f"timed out after {timeout}s"
    result.total_s = time.monotonic() - start
    return result


def get_json(base_url: str, path: str, timeout: float = 10.0) -> dict:
    with urllib.request.urlopen(base_url.rstrip("/") + path, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


# ── test case schema ─────────────────────────────────────────────────────

@dataclass
class Turn:
    message: str
    new_session: bool = False          # start a fresh session before sending this turn
    expect_any_of: list[list[str]] = field(default_factory=list)
    # ^ satisfied if ALL tool names in at least one inner list were called this turn.
    #   [] means "no tool-call requirement" (still checked against `forbid`).
    forbid: list[str] = field(default_factory=list)
    # ^ tool names that must NOT be called this turn (catches overuse / hallucinated tools)
    answer_contains_any: list[str] = field(default_factory=list)
    # ^ case-insensitive substring check against the final answer text; [] = no check
    manual_review: bool = False
    # ^ script records the transcript but doesn't auto-pass/fail (e.g. hallucination checks
    #   aren't reliably regexable -- read the transcript yourself)


@dataclass
class Case:
    id: str
    description: str
    turns: list[Turn]
    agent: str = "funes"
    requires_agent: Optional[str] = None   # skip whole case if this agent isn't loaded
    tags: list[str] = field(default_factory=list)  # e.g. "web", "slow" for filtering


CASES: list[Case] = [
    Case(
        id="no_tool_trivial",
        description="Trivial arithmetic shouldn't reach for any tool (overuse check).",
        turns=[
            Turn(
                message="What is 17 * 4? Just the number.",
                new_session=True,
                expect_any_of=[],
                forbid=["web_search", "web_fetch", "execute_shell", "read_file", "write_file"],
                answer_contains_any=["68"],
            )
        ],
    ),
    Case(
        id="remember_basic",
        description="A stated personal fact should trigger `remember`.",
        turns=[
            Turn(
                message="My favorite programming language is Rust and I've been using it since 2019.",
                new_session=True,
                expect_any_of=[["remember"]],
            )
        ],
    ),
    Case(
        id="recall_after_remember",
        description=(
            "A fact taught in one session should be recoverable in a brand new session "
            "-- via automatic recall injection and/or an explicit `recall` call. This is "
            "Funes' core feature, so it's graded on the ANSWER, not on which tool fired."
        ),
        turns=[
            Turn(
                message="Remember this: my dog's name is Zorbax and he is a corgi.",
                new_session=True,
                expect_any_of=[["remember"]],
            ),
            Turn(
                message="What's my dog's name?",
                new_session=True,   # fresh session -- can't lean on conversation history
                answer_contains_any=["zorbax"],
            ),
        ],
    ),
    Case(
        id="file_roundtrip",
        description="write_file then read_file should round-trip exact content across sessions.",
        turns=[
            Turn(
                message=(
                    "Save a note to a file called bench_note.txt in your workspace with "
                    "exactly this content: The quick brown fox jumps over 42 lazy benchmarks."
                ),
                new_session=True,
                expect_any_of=[["write_file"]],
            ),
            Turn(
                message="Read the file bench_note.txt from your workspace and tell me exactly what it says.",
                new_session=True,
                expect_any_of=[["read_file"]],
                answer_contains_any=["42", "quick brown fox"],
            ),
        ],
    ),
    Case(
        id="web_search_current",
        description="A question needing current/external info should trigger web_search.",
        tags=["web"],
        turns=[
            Turn(
                message="Search the web and tell me one headline from a major tech news site today.",
                new_session=True,
                expect_any_of=[["web_search"]],
            )
        ],
    ),
    Case(
        id="delegate_operator_domain",
        description=(
            "A question outside funes' own toolset (disk space) should be delegated, "
            "not guessed at directly."
        ),
        requires_agent="operator",
        turns=[
            Turn(
                message="How much free disk space do you have on this machine right now?",
                new_session=True,
                expect_any_of=[["delegate_to_agent"]],
            )
        ],
    ),
    Case(
        id="no_hallucinated_fact",
        description=(
            "Asking about something never taught should not produce a confident, invented "
            "answer. Not reliably auto-gradable -- read the transcript."
        ),
        turns=[
            Turn(
                message="What's my mother's maiden name?",
                new_session=True,
                manual_review=True,
            )
        ],
    ),
]


# ── scoring ──────────────────────────────────────────────────────────────

def _args_blob(args: dict) -> str:
    try:
        return json.dumps(args).lower()
    except TypeError:
        return str(args).lower()


def eval_turn(turn: Turn, result: ChatResult) -> tuple[bool, list[str]]:
    reasons: list[str] = []

    if result.error and not turn.manual_review:
        reasons.append(f"transport/error event: {result.error}")
    if result.final_text.startswith(FAILURE_MARKER) and not turn.manual_review:
        reasons.append(f"harness failure: {result.final_text[:200]}")

    called = [tc["name"] for tc in result.tool_calls]

    if turn.expect_any_of:
        satisfied = any(all(name in called for name in group) for group in turn.expect_any_of)
        if not satisfied:
            alt = " OR ".join("+".join(g) for g in turn.expect_any_of)
            reasons.append(f"expected tool call(s) [{alt}], got {called or '(none)'}")

    forbidden_hit = [n for n in called if n in turn.forbid]
    if forbidden_hit:
        reasons.append(f"forbidden tool(s) called: {forbidden_hit}")

    if turn.answer_contains_any:
        haystack = result.final_text.lower()
        if not any(needle.lower() in haystack for needle in turn.answer_contains_any):
            reasons.append(
                f"answer missing expected content {turn.answer_contains_any!r}; "
                f"got: {result.final_text[:200]!r}"
            )

    if turn.manual_review:
        return True, reasons  # never auto-fails; reasons (if any) are just notes

    return (len(reasons) == 0), reasons


# ── runner ───────────────────────────────────────────────────────────────

def new_session() -> str:
    return "bench-" + uuid.uuid4().hex[:20]


def run_case(base_url: str, case: Case, timeout: float, loaded_agents: set[str]) -> dict:
    if case.requires_agent and case.requires_agent not in loaded_agents:
        return {
            "id": case.id, "description": case.description,
            "skipped": True, "skip_reason": f"agent '{case.requires_agent}' not loaded",
            "passed": None, "turns": [],
        }

    session = new_session()
    turn_reports = []
    case_passed = True
    for i, turn in enumerate(case.turns):
        if turn.new_session:
            session = new_session()
        result = chat(base_url, case.agent, session, turn.message, timeout)
        passed, reasons = eval_turn(turn, result)
        if not turn.manual_review:
            case_passed = case_passed and passed
        turn_reports.append({
            "turn": i,
            "message": turn.message,
            "manual_review": turn.manual_review,
            "passed": passed,
            "reasons": reasons,
            "tool_calls": result.tool_calls,
            "final_text": result.final_text,
            "first_event_s": round(result.first_event_s, 3) if result.first_event_s else None,
            "total_s": round(result.total_s, 3),
            "usage": result.usage,
        })
    return {
        "id": case.id, "description": case.description,
        "skipped": False, "skip_reason": None,
        "passed": case_passed, "turns": turn_reports,
    }


def print_report(report: dict) -> None:
    print(f"\n{'CASE':<28} {'RESULT':<10} DETAIL")
    print("-" * 90)
    for c in report["cases"]:
        if c["skipped"]:
            print(f"{c['id']:<28} {'SKIP':<10} {c['skip_reason']}")
            continue
        label = "PASS" if c["passed"] else "FAIL"
        detail = ""
        if not c["passed"]:
            for t in c["turns"]:
                if t["reasons"]:
                    detail = t["reasons"][0]
                    break
        note = ""
        if any(t["manual_review"] for t in c["turns"]):
            note = " (manual review -- read transcript)"
        print(f"{c['id']:<28} {label:<10} {detail}{note}")

    s = report["summary"]
    print("-" * 90)
    print(
        f"{s['passed']}/{s['graded']} passed, {s['skipped']} skipped, "
        f"{s['manual_review']} manual-review  |  "
        f"avg first-event {s['avg_first_event_s']}s, avg total {s['avg_total_s']}s"
    )
    print(f"Model: {report['status'].get('llm', {})}  Tag: {report['tag'] or '(none)'}")


def build_summary(report: dict) -> dict:
    graded = [c for c in report["cases"] if not c["skipped"]]
    passed = [c for c in graded if c["passed"]]
    skipped = [c for c in report["cases"] if c["skipped"]]
    manual = sum(1 for c in graded for t in c["turns"] if t["manual_review"])
    all_turns = [t for c in graded for t in c["turns"] if t["first_event_s"] is not None]
    avg_first = sum(t["first_event_s"] for t in all_turns) / len(all_turns) if all_turns else 0.0
    avg_total = sum(t["total_s"] for t in all_turns) / len(all_turns) if all_turns else 0.0
    return {
        "graded": len(graded), "passed": len(passed), "failed": len(graded) - len(passed),
        "skipped": len(skipped), "manual_review": manual,
        "avg_first_event_s": round(avg_first, 3), "avg_total_s": round(avg_total, 3),
    }


def compare(paths: list[str]) -> None:
    rows = []
    for p in paths:
        with open(p) as f:
            r = json.load(f)
        s = r["summary"]
        rate = f"{s['passed']}/{s['graded']}"
        model = r.get("status", {}).get("llm", {}).get("model", "?")
        rows.append((r.get("tag") or model, rate, s["avg_first_event_s"], s["avg_total_s"], p))

    print(f"\n{'TAG/MODEL':<24} {'PASS':<8} {'AVG FIRST-EVENT':<18} {'AVG TOTAL':<12} FILE")
    print("-" * 100)
    for tag, rate, first, total, p in rows:
        print(f"{tag:<24} {rate:<8} {first:<18} {total:<12} {p}")


# ── main ─────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default="http://localhost:8484", help="Funes base URL")
    ap.add_argument("--agent", default=None, help="override agent for every case (default: per-case, usually 'funes')")
    ap.add_argument("--case", action="append", default=None, help="run only this case id (repeatable)")
    ap.add_argument("--skip-web", action="store_true", help="skip cases tagged 'web' (no search/fetch backend configured)")
    ap.add_argument("--timeout", type=float, default=90.0, help="per-turn timeout in seconds (raise for slow local models)")
    ap.add_argument("--tag", default="", help="label for this run, e.g. a model name, for later comparison")
    ap.add_argument("--out", default=None, help="directory to write a JSON report (filename auto-generated)")
    ap.add_argument("--list", action="store_true", help="list available cases and exit")
    ap.add_argument("--compare", nargs="+", default=None, help="compare previously saved JSON reports instead of running")
    args = ap.parse_args()

    if args.compare:
        paths = []
        for pattern in args.compare:
            paths.extend(glob.glob(pattern))
        if not paths:
            print("No report files matched.", file=sys.stderr)
            return 1
        compare(sorted(paths))
        return 0

    if args.list:
        for c in CASES:
            print(f"{c.id:<28} {c.description}")
        return 0

    try:
        status = get_json(args.url, "/api/status")
        agents_resp = get_json(args.url, "/api/agents")
    except Exception as e:
        print(f"Could not reach Funes at {args.url}: {e}", file=sys.stderr)
        return 1

    loaded_agents = {a["name"] for a in agents_resp.get("agents", [])}

    selected = CASES
    if args.case:
        wanted = set(args.case)
        selected = [c for c in CASES if c.id in wanted]
        missing = wanted - {c.id for c in selected}
        if missing:
            print(f"Unknown case id(s): {missing}", file=sys.stderr)
            return 1
    if args.skip_web:
        selected = [c for c in selected if "web" not in c.tags]

    if args.agent:
        for c in selected:
            c.agent = args.agent

    print(f"Running {len(selected)} case(s) against {args.url} "
          f"(model: {status.get('llm', {}).get('model', '?')})...")

    report = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "url": args.url,
        "tag": args.tag,
        "status": status,
        "loaded_agents": sorted(loaded_agents),
        "cases": [],
    }
    for case in selected:
        print(f"  - {case.id}...", end=" ", flush=True)
        result = run_case(args.url, case, args.timeout, loaded_agents)
        report["cases"].append(result)
        if result["skipped"]:
            print("SKIP")
        else:
            print("PASS" if result["passed"] else "FAIL")

    report["summary"] = build_summary(report)
    print_report(report)

    if args.out:
        import os
        os.makedirs(args.out, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        tag_part = (args.tag or status.get("llm", {}).get("model", "model")).replace("/", "_")
        out_path = os.path.join(args.out, f"funes_bench_{tag_part}_{stamp}.json")
        with open(out_path, "w") as f:
            json.dump(report, f, indent=2)
        print(f"\nReport written to {out_path}")

    graded_failed = report["summary"]["failed"] > 0
    return 1 if graded_failed else 0


if __name__ == "__main__":
    sys.exit(main())
