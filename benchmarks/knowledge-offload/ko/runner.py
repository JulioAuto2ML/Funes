"""One cell = (dataset, model, condition, repeat) -> one result JSON."""
import datetime as dt
import json
import subprocess
import time
from dataclasses import asdict
from pathlib import Path

from . import conditions as C
from . import popqa
from .llm import Client, LLMError, Usage
from .scoring import score
from .store import Fact, FactStore
from . import tools as T

RESULTS = Path(__file__).resolve().parent.parent / "results"
REPO = Path(__file__).resolve().parents[3]


def git_state() -> dict:
    def g(*a):
        return subprocess.run(["git", "-C", str(REPO), *a], capture_output=True, text=True).stdout.strip()
    return {"commit": g("rev-parse", "HEAD"), "dirty": bool(g("status", "--porcelain", "--",
            "benchmarks/knowledge-offload", "src", "agents"))}


def build_store(items: list[popqa.Item]) -> FactStore:
    """The whole PopQA triple set, not just the sample: the sample's neighbours
    (same-name entities, same relation) are the distractors a real store has."""
    return FactStore(Fact(i.subj, i.prop, i.obj) for i in items)


def cell_path(tag: str, n: int, condition: str, rep: int) -> Path:
    return RESULTS / f"popqa-n{n}-{tag}-{condition.replace('+', 'plus')}-r{rep}.json"


def run_cell(*, condition: str, items, ctx: C.Context, rep: int, meta: dict,
             log=print) -> dict:
    fn = C.REGISTRY[condition]
    passes = 2 if condition == "web+cache" else 1
    rows, t_start = [], time.monotonic()
    for p in range(1, passes + 1):
        ctx.passno = p
        for i, item in enumerate(items):
            try:
                a = fn(ctx, item)
            except C.ConditionUnavailable:
                raise
            except LLMError as e:
                a = C.Answer(error=str(e))
            s = score(a.text, list(item.aliases))
            rows.append({"id": item.id, "pass": p, "pop_bucket": popqa.pop_bucket(item),
                         "prediction": a.text, **s, "usage": a.usage.as_dict(),
                         "tools": a.tools, "steps": a.steps, "nudges": a.nudges,
                         "contract_met": a.contract_met, "retrieval_hit": a.retrieval_hit,
                         "tainted": a.tainted, "error": a.error, **a.extra})
            if (i + 1) % 100 == 0:
                done = [r for r in rows if r["pass"] == p]
                log(f"  {condition} r{rep} pass{p} {i+1}/{len(items)} "
                    f"acc={sum(r['contains'] for r in done)/len(done):.3f}")
    return {**meta, "condition": condition, "repeat": rep, "seed": ctx.seed,
            "n_questions": len(items), "wall_s": round(time.monotonic() - t_start, 1),
            "finished": dt.datetime.now().isoformat(timespec="seconds"),
            "answers": rows, "summary": summarize_rows(rows)}


def summarize_rows(rows: list[dict]) -> dict:
    out = {}
    for p in sorted({r["pass"] for r in rows}):
        rs = [r for r in rows if r["pass"] == p]
        n = len(rs)
        u = Usage()
        for r in rs:
            u.add(Usage(**r["usage"]))
        out[f"pass{p}"] = {
            "n": n, "acc": sum(r["contains"] for r in rs) / n,
            "exact": sum(r["exact"] for r in rs) / n,
            "abstain": sum(r["abstained"] for r in rs) / n,
            "errors": sum(1 for r in rs if r["error"]),
            "tainted": sum(r["tainted"] for r in rs) / n,
            "lookup_rate": sum(1 for r in rs if r["tools"]) / n,
            "retrieval_hit": (sum(bool(r["retrieval_hit"]) for r in rs) / n
                              if rs[0]["retrieval_hit"] is not None else None),
            "per_answer": {"prompt_tokens": u.prompt_tokens / n,
                           "completion_tokens": u.completion_tokens / n,
                           "wall_ms": u.wall_ms / n, "calls": u.calls / n}}
    return out


def write_cell(path: Path, result: dict) -> None:
    path.parent.mkdir(exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(result, indent=1, ensure_ascii=False))
    tmp.replace(path)
