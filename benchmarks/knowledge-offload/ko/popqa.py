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
