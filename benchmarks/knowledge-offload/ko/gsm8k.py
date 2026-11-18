"""GSM8K arm of H2 (design: NOTEBOOK.md, entry "H2 / GSM8K arm: design for implementation", 2026-11-18).

Fact-free reasoning: `closed` and `store` get the same prompt and decoding; the store is the PopQA one, which
holds nothing a word problem needs. Everything frozen by that entry is a constant here; a change is a new
notebook entry, not an edit.
"""
import hashlib
import json
import random
import re
import urllib.request
from dataclasses import dataclass
from pathlib import Path

REPO = "openai/grade-school-math"
RAW = "https://raw.githubusercontent.com/" + REPO + "/{sha}/grade_school_math/data/test.jsonl"
COMMITS = "https://api.github.com/repos/" + REPO + "/commits/master"
DEFAULT_PATH = Path(__file__).resolve().parent.parent / "data" / "gsm8k_test.jsonl"
N_PROBLEMS = 1319
# Filled in from the notebook line `GSM8K_SHA256 = <hash>` once the file was downloaded twice (this machine and
# yoda) and the two hashes were equal. None = not pinned yet: nothing but `fetch` and the tests may run.
EXPECTED_SHA256 = "3730d312f6e3440559ace48831e51066acaca737f6eabec99bccb9e4b3c39d14"

N = 500                  # items of a cell
DATASET_SEED = 1         # random.Random(1).sample(all 1319, 500)
PILOT_N, PILOT_SEED = 20, 2     # 20 problems from the 819 not in the sample, never analysed
MAX_TOKENS = 512         # the pilot may raise it to 1024 for every cell (design §5), recorded in the notebook
CONDITIONS = ("closed", "store", "tool")
H2_CONDITIONS = ("closed", "store")      # `tool` is exploratory, outside H2

SYSTEM = ('You solve grade-school math word problems. Work through the problem step by step in a few short '
          'lines, then write the final answer alone on the last line in the form "#### <number>", digits only, '
          'no units.')
SYSTEM_TOOL = SYSTEM + " Use the tools you are given when they help."


@dataclass(frozen=True)
class Problem:
    id: str          # gsm8k-test-<line index>
    question: str
    gold: float
    answer: str      # the reference solution, kept for audit, never shown to a model


def sha256(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def latest_commit() -> str:
    with urllib.request.urlopen(COMMITS, timeout=30) as r:
        return json.load(r)["sha"]


def fetch(path: Path = DEFAULT_PATH, commit: str | None = None) -> tuple[Path, str]:
    """Download the test file pinned to a commit of the repository; returns (path, commit)."""
    commit = commit or latest_commit()
    path.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(RAW.format(sha=commit), timeout=60) as r:
        path.write_bytes(r.read())
    validate(path)
    return path, commit


def _gold(answer: str) -> float:
    return float(answer.split("####")[1].strip().replace(",", ""))


def validate(path: Path) -> int:
    """1,319 lines, every line parses, every `answer` has exactly one `####`."""
    n = 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            row = json.loads(line)
            if row["answer"].count("####") != 1:
                raise ValueError(f"line {n + 1}: {row['answer'].count('####')} `####` in answer")
            _gold(row["answer"])
            n += 1
    if n != N_PROBLEMS:
        raise ValueError(f"{path}: {n} problems, expected {N_PROBLEMS}")
    return n


def check_pinned(path: Path) -> str:
    """The hash of the file, refusing a file that is not the pinned one."""
    h = sha256(path)
    if EXPECTED_SHA256 is None:
        raise ValueError("GSM8K_SHA256 is not pinned in ko/gsm8k.py yet (download on two machines, compare, pin)")
    if h != EXPECTED_SHA256:
        raise ValueError(f"{path}: sha256 {h} differs from the pinned {EXPECTED_SHA256}")
    return h


def load(path: Path = DEFAULT_PATH) -> list[Problem]:
    out = []
    with open(path, encoding="utf-8") as f:
        for i, line in enumerate(f):
            row = json.loads(line)
            out.append(Problem(f"gsm8k-test-{i}", row["question"], _gold(row["answer"]), row["answer"]))
    return out


def sample(problems: list[Problem], n: int = N, seed: int = DATASET_SEED) -> list[Problem]:
    return random.Random(seed).sample(problems, min(n, len(problems)))


def pilot_sample(problems: list[Problem]) -> list[Problem]:
    """20 problems from the ones not in the n=500 sample (seed 2), only for the pilot."""
    used = {p.id for p in sample(problems)}
    rest = [p for p in problems if p.id not in used]
    return random.Random(PILOT_SEED).sample(rest, PILOT_N)


# ---- answer extraction (primary metric, design §6) ----
_STRICT = re.compile(r"####\s*(-?[\d,]*\.?\d+)")
_NUM = re.compile(r"-?\d[\d,]*(?:\.\d+)?")


def _to_float(s: str) -> float | None:
    s = s.replace(",", "").replace("$", "").replace("%", "").strip().rstrip(".")
    try:
        return float(s)
    except ValueError:
        return None


def extract(text: str, finish_reason: str = "stop") -> tuple[str | None, str, float | None]:
    """(raw extraction, mode, value). Modes: strict (last `#### n`), fallback (last number), none
    (no number, or the reply hit the length limit with no `####` line). `none` is wrong, never dropped."""
    m = _STRICT.findall(text or "")
    if m:
        return m[-1], "strict", _to_float(m[-1])
    if finish_reason == "length":
        return None, "none", None
    nums = [x.group(0) for x in _NUM.finditer(text or "")]
    if nums:
        return nums[-1], "fallback", _to_float(nums[-1])
    return None, "none", None


def is_correct(value: float | None, gold: float) -> bool:
    return value is not None and abs(value - gold) < 1e-6


def score(text: str, finish_reason: str, gold: float) -> dict:
    raw, mode, value = extract(text, finish_reason)
    return {"extracted": raw, "mode": mode, "value": value, "correct": is_correct(value, gold),
            "correct_strict": mode == "strict" and is_correct(value, gold)}


def cell_path(results: Path, tag: str, cond: str, rep: int, *, n: int = N, seed: int = DATASET_SEED,
              pilot: bool = False) -> Path:
    head = f"gsm8k-pilot-n{n}-s{seed}" if pilot else f"gsm8k-n{n}-s{seed}"
    return results / f"{head}-{tag}-{cond}-r{rep}.json"


# ---- running a cell ----
def _closed_or_store(ctx, p: Problem, with_store: bool):
    from . import conditions as C
    facts = []
    user = p.question
    if with_store:
        found = ctx.store.search(p.question, k=C.K)
        facts = [f.text() for f in found]
        listing = "\n".join(f"- {t}" for t in facts) or "- (nothing retrieved)"
        user = f"Facts:\n{listing}\n\nQuestion: {p.question}"
    r = ctx.client.chat([{"role": "system", "content": SYSTEM}, {"role": "user", "content": user}],
                        seed=ctx.seed, max_tokens=ctx.max_tokens)
    return C.Answer(text=r.text, usage=r.usage, steps=1,
                    extra={"finish_reason": r.finish_reason, "facts": facts})


def answer(ctx, cond: str, p: Problem):
    from . import conditions as C, tools as T
    if cond == "closed":
        return _closed_or_store(ctx, p, False)
    if cond == "store":
        return _closed_or_store(ctx, p, True)
    if cond == "tool":
        return C.tool_loop(ctx, p.question, [T.LOOKUP], {"lookup": lambda a: T.run_lookup(ctx.store, a, None)},
                           system=SYSTEM_TOOL, max_tokens=ctx.max_tokens)
    raise ValueError(f"unknown GSM8K condition {cond}")


def run_cell(*, cond: str, problems, ctx, rep: int, meta: dict, log=print) -> dict:
    import datetime as dt
    import time
    from .llm import LLMError
    from . import conditions as C
    rows, t0 = [], time.monotonic()
    for i, p in enumerate(problems):
        try:
            a = answer(ctx, cond, p)
        except LLMError as e:
            a = C.Answer(error=str(e), extra={"finish_reason": ""})
        s = score(a.text, a.extra.get("finish_reason", ""), p.gold)
        rows.append({"id": p.id, "gold": p.gold, "prediction": a.text, **s,
                     "finish_reason": a.extra.get("finish_reason", ""), "facts": a.extra.get("facts"),
                     "usage": a.usage.as_dict(), "tools": a.tools, "steps": a.steps, "error": a.error})
        if (i + 1) % 100 == 0:
            log(f"  {cond} r{rep} {i + 1}/{len(problems)} acc={sum(r['correct'] for r in rows) / len(rows):.3f}")
    return {**meta, "condition": cond, "repeat": rep, "seed": ctx.seed, "n_questions": len(problems),
            "max_tokens": ctx.max_tokens, "wall_s": round(time.monotonic() - t0, 1),
            "finished": dt.datetime.now().isoformat(timespec="seconds"),
            "answers": rows, "summary": summarize(rows)}


def summarize(rows: list[dict]) -> dict:
    n = len(rows)
    toks = [r["usage"]["completion_tokens"] for r in rows]
    modes = {m: sum(1 for r in rows if r["mode"] == m) / n for m in ("strict", "fallback", "none")}
    return {"n": n, "acc": sum(r["correct"] for r in rows) / n,
            "acc_strict": sum(r["correct_strict"] for r in rows) / n, "modes": modes,
            "format_dominated": modes["none"] >= 0.20,
            "at_limit": sum(1 for r in rows if r["finish_reason"] == "length") / n,
            "completion_tokens_mean": sum(toks) / n, "completion_tokens_max": max(toks),
            "lookup_rate": sum(1 for r in rows if r["tools"]) / n,
            "errors": sum(1 for r in rows if r["error"])}
