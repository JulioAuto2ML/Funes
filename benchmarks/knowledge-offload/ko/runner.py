"""One cell = (dataset, model, condition, repeat) -> one result JSON."""
import datetime as dt
import gzip
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
    return FactStore(Fact(*t) for t in popqa.facts(items))


def pool_texts(items: list[popqa.Item]) -> list[str]:
    """The same triples as `build_store`, as the text `funes` is given to remember."""
    return [Fact(*t).text() for t in popqa.facts(items)]


def cell_path(tag: str, n: int, condition: str, rep: int, *, sample: str = "random",
              dataset_seed: int = 0, pool: int | None = None, variant: str = "default") -> Path:
    """Defaults keep the E0 names (`popqa-n1000-<tag>-<cond>-r<k>.json`) as they
    were; a stratified sample and a non-default funes pool say so in the name,
    so a new row never overwrites an old one."""
    head = "popqa" if sample == "random" else f"popqa-{sample}{dataset_seed}"
    cond = condition.replace("+", "plus") + (f"-pool{pool}" if pool and condition == "funes" else "")
    if variant != "default":
        cond += f"-{variant}"
    return RESULTS / f"{head}-n{n}-{tag}-{cond}-r{rep}.json"


def payload_dir(tag: str, condition: str, rep: int, passno: int, *, sample: str = "random",
                dataset_seed: int = 0, variant: str = "default") -> Path:
    """results/payloads/<run>/<condition>-r<rep>-p<pass>/<question id>.json.gz"""
    run = f"{tag}" if sample == "random" else f"{tag}-{sample}{dataset_seed}"
    cond = condition.replace("+", "plus") + ("" if variant == "default" else f"-{variant}")
    return RESULTS / "payloads" / run / f"{cond}-r{rep}-p{passno}"


def write_payloads(directory: Path, qid: str, payloads: list) -> None:
    if not payloads:
        return
    directory.mkdir(parents=True, exist_ok=True)
    with gzip.open(directory / f"{qid}.json.gz", "wt", encoding="utf-8") as f:
        json.dump({"id": qid, "payloads": payloads}, f, ensure_ascii=False)


def run_cell(*, condition: str, items, ctx: C.Context, rep: int, meta: dict,
             log=print, candidates: dict | None = None, payloads_to=None) -> dict:
    """`candidates` (popqa.candidate_sets) turns on the `hedge` flag;
    `payloads_to(passno) -> Path` is where raw web payloads go, one file per question."""
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
            s = score(a.text, list(item.aliases),
                      None if candidates is None else candidates.get((item.subj, item.prop)))
            if payloads_to and a.payloads:
                write_payloads(payloads_to(p), item.id, a.payloads)
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
            **({"hedge_rate": sum(r["hedge"] for r in rs) / n,
                "acc_nohedge": sum(r["contains"] and not r["hedge"] for r in rs) / n}
               if all("hedge" in r for r in rs) else {}),
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
