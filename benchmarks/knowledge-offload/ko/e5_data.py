"""E5 training-data builder (E5_DESIGN.md section 3). No model writes any of it, nothing here touches a
GPU, a network or yoda.

Order of the work, and the order `build` prints it in: (1) the eval sample, reused from `popqa` and not
re-sampled; (2) the held-out relation draw, printed and written before anything else; (3) the disjoint
split (dev, training pool); (4) lookup (L1) and control (L0) examples; (5) anchors, from a file of prompts
and a file of the *base model's own* answers made later on the GPU.

Everything frozen by the design is a constant here; a change is a notebook entry, not a quiet edit.
Tokenisation happens at train time: rows carry messages, the tools block and a per-message mask
description, never token ids.
"""
import hashlib
import json
import math
import random
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from . import conditions as C, gsm8k, popqa, tools as T
from .llm import Client
from .popqa import Item
from .scoring import normalize
from .store import Fact, FactStore

# ---- frozen by E5_DESIGN.md ----
EVAL_N, EVAL_SEED = 1000, 1                  # the sample every committed PopQA cell used (stratified, unambiguous)
HELDOUT_N = 3                                # relation units never seen in training
HELDOUT_MIN_EVAL = 40                        # a unit needs this many eval items to be drawn
HELDOUT_MIN_TOTAL = 150                      # the three together; below it, redraw once
HELDOUT_SEEDS = (5, 6)                       # first draw, the one redraw
UNIT_OF = {"capital of": "capital"}          # two relations, one held-out unit
EXCLUDED_UNITS = frozenset({"color"})        # 5 eval items
DEV_N = 300
N_LOOKUP_START, N_LOOKUP_RETRAIN = 1000, 3000
ANCHOR_RATIO = 0.5                           # 500 anchors per 1,000 lookups: one third of the mix
# ---- choices the design leaves open (listed in the hand-over report) ----
DEV_SEED, LOOKUP_SEED, ANCHOR_SEED = 2, 3, 4
TOOL_CALL_ID = "call_0"
TEMPLATE_KWARGS = {"enable_thinking": False}   # what llm.Client.chat sends with every request
ANCHOR_SYSTEM = gsm8k.SYSTEM                   # the prompt of the GSM8K `closed` eval cell
ARMS = ("L1", "L0")

DEFAULT_CELL = (Path(__file__).resolve().parent.parent / "results"
                / "popqa-stratified1u-n1000-qwen3.5-2b-iq4_xs-closed-r0.json")


class E5DataError(RuntimeError):
    """The data cannot be built as the design says. Never worked around quietly."""


# ---- loading ----
def load_s_uris(path: Path = popqa.DEFAULT_PATH) -> dict[str, str]:
    """id -> subject URI. `popqa.Item` does not carry it (and is frozen for every committed cell), so it
    is read from the same file here."""
    import csv
    out = {}
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if not row.get("s_uri"):
                raise E5DataError(f"row {row.get('id')} has no s_uri: shared-subject disjointness cannot be asserted")
            out[row["id"]] = row["s_uri"]
    return out


def eval_items(all_items: list[Item]) -> list[Item]:
    """The frozen eval sample, by the code path `run.py` used (`--exclude-ambiguous --sample stratified
    --dataset-seed 1 --n 1000`). Not a new sample."""
    return popqa.stratified_sample(popqa.unambiguous(all_items), EVAL_N, EVAL_SEED)


def check_against_cell(items: list[Item], cell_path: Path = DEFAULT_CELL) -> bool | None:
    """True/False: same ids as a committed result cell. None when the cell is not on this machine."""
    if not Path(cell_path).exists():
        return None
    cell = json.loads(Path(cell_path).read_text(encoding="utf-8"))
    return {a["id"] for a in cell["answers"]} == {i.id for i in items}


# ---- the held-out draw ----
def unit_of(prop: str) -> str:
    return UNIT_OF.get(prop, prop)


def unit_counts(items: list[Item]) -> dict[str, int]:
    return dict(sorted(Counter(unit_of(i.prop) for i in items).items()))


@dataclass(frozen=True)
class HeldOut:
    units: tuple[str, ...]
    seed: int
    eval_total: int
    attempts: tuple[dict, ...]       # every draw made, with its eval total: the redraw is logged, not hidden
    below_threshold: bool            # still under HELDOUT_MIN_TOTAL after the one redraw

    def as_dict(self) -> dict:
        return {"units": list(self.units), "seed": self.seed, "eval_total": self.eval_total,
                "attempts": list(self.attempts), "below_threshold": self.below_threshold,
                "min_eval_per_unit": HELDOUT_MIN_EVAL, "min_total": HELDOUT_MIN_TOTAL,
                "excluded_units": sorted(EXCLUDED_UNITS)}


def draw_heldout(eval_sample: list[Item]) -> HeldOut:
    """Three units from those with >= 40 eval items (capital + capital of one unit, color out), by
    `random.Random(5).sample` over the alphabetically sorted unit names; if the three hold fewer than 150
    eval items together, one redraw with `Random(6)`."""
    counts = unit_counts(eval_sample)
    eligible = sorted(u for u, c in counts.items() if c >= HELDOUT_MIN_EVAL and u not in EXCLUDED_UNITS)
    if len(eligible) < HELDOUT_N:
        raise E5DataError(f"only {len(eligible)} units have >= {HELDOUT_MIN_EVAL} eval items")
    attempts = []
    for seed in HELDOUT_SEEDS:
        units = tuple(sorted(random.Random(seed).sample(eligible, HELDOUT_N)))
        total = sum(counts[u] for u in units)
        attempts.append({"seed": seed, "units": list(units), "eval_total": total})
        if total >= HELDOUT_MIN_TOTAL:
            break
    last = attempts[-1]
    return HeldOut(tuple(last["units"]), last["seed"], last["eval_total"], tuple(attempts),
                   last["eval_total"] < HELDOUT_MIN_TOTAL)


def describe_draw(held: HeldOut, eval_sample: list[Item]) -> str:
    counts = unit_counts(eval_sample)
    raw = Counter(i.prop for i in eval_sample)
    lines = ["eval items per relation (unit in brackets when merged):"]
    for prop in sorted(raw):
        mark = "  <- HELD OUT" if unit_of(prop) in held.units else ""
        merged = f" [{unit_of(prop)}]" if unit_of(prop) != prop else ""
        lines.append(f"  {prop}{merged}: {raw[prop]}{mark}")
    lines.append("eval items per unit: " + ", ".join(f"{u}={c}" for u, c in counts.items()))
    for a in held.attempts:
        lines.append(f"draw Random({a['seed']}): {', '.join(a['units'])} = {a['eval_total']} eval items")
    if len(held.attempts) > 1:
        lines.append(f"REDRAW: the first draw held < {HELDOUT_MIN_TOTAL} eval items")
    lines.append(f"held out: {', '.join(held.units)} = {held.eval_total} eval items"
                 + (f"  WARNING: still < {HELDOUT_MIN_TOTAL} after the one redraw" if held.below_threshold else ""))
    return "\n".join(lines)


# ---- the split ----
@dataclass(frozen=True)
class Split:
    eval: tuple[Item, ...]
    dev: tuple[Item, ...]
    pool: tuple[Item, ...]           # what training examples are drawn from
    held_out: tuple[str, ...]


def build_split(all_items: list[Item], eval_sample: list[Item], held: HeldOut, s_uri: dict[str, str]) -> Split:
    """Training pool = unambiguous triples that are not an eval id, do not share a subject URI with one,
    and are not in a held-out unit. The dev set is drawn from the same candidates and the pool then also
    drops every triple sharing a subject URI with a dev item, so dev and training are disjoint too.
    Unambiguous only: the eval is, and an answer "the first alias" is only right when there is one object."""
    eval_ids = {i.id for i in eval_sample}
    eval_uris = {s_uri[i.id] for i in eval_sample}
    cands = [i for i in popqa.unambiguous(all_items)
             if i.id not in eval_ids and s_uri[i.id] not in eval_uris and unit_of(i.prop) not in held.units]
    dev = popqa.stratified_sample(cands, DEV_N, DEV_SEED)
    dev_uris = {s_uri[i.id] for i in dev}
    pool = [i for i in cands if s_uri[i.id] not in dev_uris]
    split = Split(tuple(eval_sample), tuple(dev), tuple(pool), held.units)
    assert_disjoint(split, s_uri)
    return split


def assert_disjoint(split: Split, s_uri: dict[str, str]) -> None:
    """Raises, never `assert` (stripped under -O). Checked on every build, and again by the tests."""
    eval_ids = {i.id for i in split.eval}
    eval_uris = {s_uri[i] for i in eval_ids}
    dev_ids = {i.id for i in split.dev}
    dev_uris = {s_uri[i] for i in dev_ids}
    for name, part in (("dev", split.dev), ("train", split.pool)):
        if eval_ids & {i.id for i in part}:
            raise E5DataError(f"{name} contains an eval id")
        if eval_uris & {s_uri[i.id] for i in part}:
            raise E5DataError(f"{name} shares a subject URI with an eval item")
        if any(unit_of(i.prop) in split.held_out for i in part):
            raise E5DataError(f"{name} contains a held-out relation")
    if dev_ids & {i.id for i in split.pool} or dev_uris & {s_uri[i.id] for i in split.pool}:
        raise E5DataError("dev and train share an id or a subject URI")


# ---- popularity-bucket sampling ----
def bucket_targets(n: int, eval_sample: list[Item]) -> dict[str, int]:
    """n examples split over buckets in the eval sample's proportions (largest remainder), so n = 1000
    gives exactly the eval counts (about 195 per 1e1-1e5)."""
    counts = Counter(popqa.pop_bucket(i) for i in eval_sample)
    total = sum(counts.values())
    exact = {b: n * c / total for b, c in counts.items()}
    out = {b: math.floor(v) for b, v in exact.items()}
    order = sorted(exact, key=lambda b: (-(exact[b] - out[b]), b))
    for b in order[: n - sum(out.values())]:
        out[b] += 1
    return dict(sorted(out.items()))


def pool_bucket_counts(pool, eval_sample: list[Item]) -> dict[str, dict]:
    have = Counter(popqa.pop_bucket(i) for i in pool)
    ev = Counter(popqa.pop_bucket(i) for i in eval_sample)
    return {b: {"pool": have.get(b, 0), "eval": ev.get(b, 0)} for b in sorted(set(have) | set(ev))}


def sample_by_bucket(pool, targets: dict[str, int], seed: int) -> tuple[list[Item], dict[str, dict]]:
    """Per bucket: the target, or everything the pool holds when it is smaller. A short bucket is reported
    in the second return value, never padded from another one."""
    rng = random.Random(seed)
    by: dict[str, list[Item]] = {}
    for i in pool:
        by.setdefault(popqa.pop_bucket(i), []).append(i)
    taken, report = [], {}
    for b in sorted(set(targets) | set(by)):
        want, got = targets.get(b, 0), by.get(b, [])
        k = min(want, len(got))
        taken.extend(rng.sample(got, k))
        report[b] = {"target": want, "pool": len(got), "taken": k, "short": want - k}
    rng.shuffle(taken)
    return taken, report


# ---- examples ----
def tool_call_message(entity: str, relation: str) -> dict:
    """The assistant turn, shaped like a llama-server reply (arguments is a JSON *string*)."""
    return {"role": "assistant", "content": "", "tool_calls": [{
        "id": TOOL_CALL_ID, "type": "function",
        "function": {"name": "lookup",
                     "arguments": json.dumps({"entity": entity, "relation": relation}, ensure_ascii=False)}}]}


def _mask(messages: list[dict]) -> list[dict]:
    """Loss on assistant turns only (the call, the final answer); prompt and tool results masked."""
    what = {"system": "prompt", "user": "prompt", "tool": "tool result"}
    out = []
    for i, m in enumerate(messages):
        if m["role"] == "assistant":
            kind = "call" if m.get("tool_calls") else "answer"
        else:
            kind = what[m["role"]]
        out.append({"index": i, "role": m["role"], "what": kind, "train": m["role"] == "assistant"})
    return out


def _row(row_id: str, arm: str, kind: str, messages: list[dict], with_tools: bool) -> dict:
    return {"id": row_id, "arm": arm, "kind": kind, "messages": messages,
            "tools": [json.loads(json.dumps(T.LOOKUP))] if with_tools else None,
            "chat_template_kwargs": dict(TEMPLATE_KWARGS), "mask": _mask(messages)}


def _grounded(result: str, item: Item) -> bool:
    return Fact(item.subj, item.prop, item.obj).text() in result.splitlines()


def lookup_row(item: Item, store: FactStore) -> dict | None:
    """question -> lookup call -> result (the harness's own `run_lookup`, K = 5, all distractors) -> the
    object's first alias. None when the result does not contain the answer: that example would teach
    the model to answer something the tool did not show."""
    args = {"entity": item.subj, "relation": item.prop}
    result = T.run_lookup(store, args)
    if not _grounded(result, item):
        return None
    messages = [{"role": "system", "content": C.SYSTEM_TOOL},
                {"role": "user", "content": item.question},
                tool_call_message(item.subj, item.prop),
                {"role": "tool", "tool_call_id": TOOL_CALL_ID, "content": result},
                {"role": "assistant", "content": item.aliases[0]}]
    return _row(f"L1:lookup:{item.id}", "L1", "lookup", messages, True)


def direct_row(item: Item) -> dict:
    """L0: the same system prompt, question and answer; no tools block, no call."""
    messages = [{"role": "system", "content": C.SYSTEM_TOOL},
                {"role": "user", "content": item.question},
                {"role": "assistant", "content": item.aliases[0]}]
    return _row(f"L0:lookup:{item.id}", "L0", "lookup", messages, False)


def build_pair(items: list[Item], store: FactStore) -> tuple[list[dict], list[dict], list[str]]:
    """L1 rows, L0 rows over exactly the same items, and the ids dropped as ungrounded."""
    l1, l0, dropped = [], [], []
    for item in items:
        row = lookup_row(item, store)
        if row is None:
            dropped.append(item.id)
            continue
        verify_roundtrip(row)
        l1.append(row)
        l0.append(direct_row(item))
    return l1, l0, dropped


# ---- the call must parse with the harness's own parser ----
class _Replay(Client):
    """llm.Client with the HTTP hop replaced by a canned completion: the parser under test is the
    harness's own `Client.chat`."""

    def __init__(self, message: dict):
        super().__init__("http://replay.invalid")
        self._message = message

    def _json(self, path, body=None):
        return {"choices": [{"message": self._message, "finish_reason": "tool_calls"}]}


def parse_with_harness(message: dict) -> list[dict]:
    return _Replay(message).chat([{"role": "user", "content": "x"}]).tool_calls


def verify_roundtrip(row: dict) -> None:
    """The rendered call parses with `llm.py` to exactly the lookup the row says it makes."""
    call = next((m for m in row["messages"] if m.get("tool_calls")), None)
    if call is None:
        return
    parsed = parse_with_harness(call)
    try:
        want = json.loads(call["tool_calls"][0]["function"]["arguments"])
    except json.JSONDecodeError:
        want = None
    if want is None or len(parsed) != 1 or parsed[0]["name"] != "lookup" or parsed[0]["arguments"] != want:
        raise E5DataError(f"{row['id']}: the call does not round-trip through llm.py: {parsed}")


def template_messages(row: dict) -> list[dict]:
    """The same messages with call arguments as dicts, which is what Hugging Face chat templates iterate
    over. The stored form (a JSON string) is what llama-server returns and eval replays."""
    out = []
    for m in row["messages"]:
        if m.get("tool_calls"):
            calls = [{**c, "function": {**c["function"], "arguments": json.loads(c["function"]["arguments"])}}
                     for c in m["tool_calls"]]
            m = {**m, "tool_calls": calls}
        out.append(m)
    return out


# ---- anchors ----
def _read_jsonl(path: Path) -> list[dict]:
    try:
        return [json.loads(l) for l in Path(path).read_text(encoding="utf-8").splitlines() if l.strip()]
    except (OSError, json.JSONDecodeError) as e:
        raise E5DataError(f"{path}: {e}") from e


def build_anchor_rows(prompts_path: Path, answers_path: Path, n: int, seed: int = ANCHOR_SEED,
                      test_path: Path | None = None) -> dict[str, list[dict]]:
    """Anchor rows from GSM8K `train.jsonl` questions (`{"question": ...}` per line) and the base model's
    own answers (`{"idx": <line index in the prompts file>, "text": ...}` per line, produced later on the
    GPU). Nothing is generated here. Answers are kept right or wrong; an empty one is skipped and counted
    (`skipped`). Raises if fewer than n usable, or if a question is also in the GSM8K *test* file."""
    prompts = _read_jsonl(prompts_path)
    answers = {}
    for a in _read_jsonl(answers_path):
        if not isinstance(a.get("idx"), int) or not 0 <= a["idx"] < len(prompts) or not isinstance(a.get("text"), str):
            raise E5DataError(f"{answers_path}: bad answer row {a!r}")
        answers[a["idx"]] = a["text"].strip()
    usable = sorted(i for i, t in answers.items() if t)
    if len(usable) < n:
        raise E5DataError(f"{len(usable)} usable base answers for {n} anchors "
                          f"({len(answers) - len(usable)} empty): not padded")
    chosen = sorted(random.Random(seed).sample(usable, n))
    if test_path is not None and Path(test_path).exists():
        test_q = {normalize(r["question"]) for r in _read_jsonl(test_path)}
        leaked = [i for i in chosen if normalize(prompts[i]["question"]) in test_q]
        if leaked:
            raise E5DataError(f"{len(leaked)} anchor prompts are GSM8K test questions (first idx {leaked[0]})")
    out = {arm: [] for arm in ARMS}
    for i in chosen:
        messages = [{"role": "system", "content": ANCHOR_SYSTEM},
                    {"role": "user", "content": prompts[i]["question"]},
                    {"role": "assistant", "content": answers[i]}]
        for arm in ARMS:
            out[arm].append(_row(f"{arm}:anchor:{i}", arm, "anchor", messages, arm == "L1"))
    return out


# ---- writing ----
def dumps(obj) -> str:
    return json.dumps(obj, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def write_jsonl(path: Path, rows: list[dict]) -> str:
    """Byte-deterministic (sorted keys, fixed separators, LF); returns the file's sha256."""
    data = "".join(dumps(r) + "\n" for r in rows).encode("utf-8")
    Path(path).write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, obj) -> str:
    data = (json.dumps(obj, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")
    Path(path).write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def item_record(i: Item, s_uri: dict[str, str]) -> dict:
    return {"id": i.id, "question": i.question, "subj": i.subj, "prop": i.prop, "aliases": list(i.aliases),
            "bucket": popqa.pop_bucket(i), "s_uri": s_uri[i.id]}


def describe_buckets(buckets: dict[str, dict], counts: dict[str, dict]) -> str:
    lines = ["popularity buckets (eval | pool | target | taken):"]
    for b, r in buckets.items():
        lines.append(f"  {b}: {counts.get(b, {}).get('eval', 0)} | {r['pool']} | {r['target']} | {r['taken']}"
                     + (f"  SHORT by {r['short']}" if r["short"] else ""))
    return "\n".join(lines)


def build(data_path: Path, out_dir: Path, n_lookup: int = N_LOOKUP_START, log=print,
          cell_path: Path = DEFAULT_CELL) -> dict:
    """Everything except anchors and paraphrases. Prints the held-out draw first. Returns the manifest."""
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    all_items = popqa.load(data_path)
    s_uri = load_s_uris(data_path)
    sample = eval_items(all_items)
    same = check_against_cell(sample, cell_path)
    held = draw_heldout(sample)
    text = describe_draw(held, sample)
    log(text)
    files = {"heldout_draw.json": write_json(out / "heldout_draw.json", {
        **held.as_dict(), "eval_counts_by_relation": dict(sorted(Counter(i.prop for i in sample).items())),
        "eval_counts_by_unit": unit_counts(sample), "text": text})}
    split = build_split(all_items, sample, held, s_uri)
    targets = bucket_targets(n_lookup, sample)
    chosen, buckets = sample_by_bucket(split.pool, targets, LOOKUP_SEED)
    log(describe_buckets(buckets, pool_bucket_counts(split.pool, sample)))
    l1, l0, dropped = build_pair(chosen, runner_store(all_items))
    files["eval_ids.json"] = write_json(out / "eval_ids.json", [i.id for i in sample])
    files["dev_items.jsonl"] = write_jsonl(out / "dev_items.jsonl", [item_record(i, s_uri) for i in split.dev])
    files[f"lookup_L1_n{n_lookup}.jsonl"] = write_jsonl(out / f"lookup_L1_n{n_lookup}.jsonl", l1)
    files[f"lookup_L0_n{n_lookup}.jsonl"] = write_jsonl(out / f"lookup_L0_n{n_lookup}.jsonl", l0)
    manifest = {"dataset_sha256": popqa.sha256(data_path), "n_triples": len(all_items), "eval_n": len(sample),
                "eval_matches_committed_cell": same, "heldout": held.as_dict(), "n_dev": len(split.dev),
                "n_pool": len(split.pool), "n_lookup_requested": n_lookup, "n_lookup_written": len(l1),
                "dropped_ungrounded": dropped, "buckets": buckets,
                "pool_by_bucket": pool_bucket_counts(split.pool, sample),
                "seeds": {"dev": DEV_SEED, "lookup": LOOKUP_SEED, "anchor": ANCHOR_SEED},
                "files_sha256": files}
    files["manifest.json"] = write_json(out / "manifest.json", manifest)
    log("sha256:\n" + "\n".join(f"  {h}  {n}" for n, h in sorted(files.items())))
    return manifest


def runner_store(all_items: list[Item]) -> FactStore:
    """The eval's store: every one of the 14,267 triples (same builder as `run.py`)."""
    from .runner import build_store
    return build_store(all_items)
