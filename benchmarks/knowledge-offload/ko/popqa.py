"""PopQA (Mallen et al. 2023, arXiv:2212.10511): load, fingerprint, sample."""
import csv
import hashlib
import json
import math
import random
import urllib.request
from dataclasses import dataclass
from pathlib import Path

URL = "https://huggingface.co/datasets/akariasai/PopQA/resolve/main/test.tsv"
DEFAULT_PATH = Path(__file__).resolve().parent.parent / "data" / "popqa_test.tsv"


@dataclass(frozen=True)
class Item:
    id: str
    question: str
    subj: str
    prop: str
    obj: str
    aliases: tuple[str, ...]   # every accepted answer, gold object included
    s_pop: int


def fetch(path: Path = DEFAULT_PATH) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(URL, timeout=60) as r:
        path.write_bytes(r.read())
    return path


def sha256(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(path: Path = DEFAULT_PATH) -> list[Item]:
    items = []
    with open(path, encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            try:
                aliases = json.loads(row["possible_answers"])
            except (KeyError, json.JSONDecodeError):
                aliases = [row["obj"]]
            items.append(Item(
                id=row["id"], question=row["question"], subj=row["subj"],
                prop=row["prop"], obj=row["obj"],
                aliases=tuple(dict.fromkeys([row["obj"], *aliases])),
                s_pop=int(float(row.get("s_pop") or 0))))
    return items


def sample(items: list[Item], n: int, seed: int) -> list[Item]:
    """A fixed random sample: same items for every model, condition and repeat.
    `seed` is the *dataset* seed, independent of the decoding seed."""
    return random.Random(seed).sample(items, min(n, len(items)))


def pop_bucket(item: Item) -> str:
    """Order-of-magnitude popularity bucket, for the long-tail facet."""
    p = max(item.s_pop, 1)
    return f"1e{int(math.log10(p))}"


def candidate_sets(items: list[Item]) -> dict:
    """(subject, property) -> one alias tuple per distinct object in the whole
    file. A subject name shared by several entities has several."""
    from .scoring import normalize
    seen: dict = {}
    for i in items:
        d = seen.setdefault((i.subj, i.prop), {})
        d.setdefault(normalize(i.obj), set()).update(i.aliases)
    return {k: tuple(tuple(sorted(a)) for a in v.values()) for k, v in seen.items()}


def stratified_sample(items: list[Item], n: int, seed: int) -> list[Item]:
    """Equal items per popularity bucket (`pop_bucket`), so the long tail is not
    one percent of the table. A bucket with fewer items than its share gives
    all it has and the rest is spread over the others. Deterministic in `seed`."""
    rng = random.Random(seed)
    by: dict[str, list[Item]] = {}
    for i in items:
        by.setdefault(pop_bucket(i), []).append(i)
    left, take = n, {b: 0 for b in by}
    open_ = sorted(by)
    while left > 0 and open_:
        share = max(left // len(open_), 1)
        for b in list(open_):
            g = min(share, len(by[b]) - take[b], left)
            take[b] += g
            left -= g
            if take[b] >= len(by[b]):
                open_.remove(b)
            if left == 0:
                break
    out = []
    for b in sorted(by):
        out.extend(rng.sample(by[b], take[b]))
    rng.shuffle(out)
    return out


def facts(items: list[Item]) -> list[tuple[str, str, str]]:
    """The one list of triples every fact-holding condition is built from:
    `store` (FTS5) and `funes` (scratch memory pool) both. Two builders would
    let the two drift, and then funes-vs-store stops being one variable."""
    return [(i.subj, i.prop, i.obj) for i in items]
