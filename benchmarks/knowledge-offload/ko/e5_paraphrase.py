"""Paraphrase set P (E5_DESIGN.md section 2, eval 5): scaffolding only.

Julio / Opus review the templates; nothing here writes one. `e5_paraphrase_templates.json` is empty until
they are written, and `load_templates` refuses an empty file, so a missing file never turns into a quiet
run on PopQA's own wording. The templates are hashed (sha256 of the canonical JSON of the `templates`
mapping) and the expected hash is passed in from the notebook line recorded *before training*.
"""
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

from .e5_data import E5DataError
from .popqa import Item

TEMPLATES_PATH = Path(__file__).resolve().parent.parent / "e5_paraphrase_templates.json"
PER_RELATION = 2
PLACEHOLDER = "{subj}"
PARAPHRASE_N = 200       # the first 200 eval items, as the ablated arm takes them


class TemplatesNotWritten(E5DataError):
    """The templates file is still the empty, documented stub."""


@dataclass(frozen=True)
class Templates:
    by_relation: dict          # PopQA `prop` -> tuple of PER_RELATION templates
    sha256: str


def canonical(templates: dict) -> bytes:
    return json.dumps(templates, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def templates_sha256(templates: dict) -> str:
    return hashlib.sha256(canonical(templates)).hexdigest()


def load_templates(path: Path = TEMPLATES_PATH, expected_sha256: str | None = None) -> Templates:
    try:
        doc = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise E5DataError(f"{path}: {e}") from e
    raw = doc.get("templates") if isinstance(doc, dict) else None
    if not isinstance(raw, dict):
        raise E5DataError(f"{path}: no `templates` mapping")
    if not raw:
        raise TemplatesNotWritten(f"{path} has no templates yet: they are written by hand and reviewed first")
    digest = templates_sha256(raw)
    if expected_sha256 is not None and digest != expected_sha256:
        raise E5DataError(f"templates sha256 {digest} != expected {expected_sha256}: edited after it was recorded")
    return Templates({k: tuple(v) for k, v in raw.items()}, digest)


def validate_shape(by_relation: dict) -> None:
    for rel, ts in by_relation.items():
        if len(ts) != PER_RELATION or len(set(ts)) != PER_RELATION:
            raise E5DataError(f"{rel!r}: need {PER_RELATION} distinct templates, got {list(ts)}")
        for t in ts:
            if not isinstance(t, str) or t.count(PLACEHOLDER) != 1 or t.replace(PLACEHOLDER, "").count("{") \
                    or t.replace(PLACEHOLDER, "").count("}"):
                raise E5DataError(f"{rel!r}: template {t!r} must hold {PLACEHOLDER} exactly once and no other braces")


def popqa_form(item: Item) -> str | None:
    """PopQA's own wording of this item, with the subject abstracted; None if the subject is not in it."""
    return item.question.replace(item.subj, PLACEHOLDER) if item.subj in item.question else None


def paraphrase_items(eval_sample: list[Item], n: int = PARAPHRASE_N) -> list[Item]:
    if len(eval_sample) < n:
        raise E5DataError(f"{len(eval_sample)} eval items, {n} needed for P")
    return list(eval_sample[:n])


def apply_templates(items: list[Item], templates: Templates) -> list[dict]:
    """One paraphrase per item, template index = position mod 2 (deterministic, balanced). Every relation
    present must have templates, and none may equal PopQA's own wording."""
    validate_shape(templates.by_relation)
    rows = []
    for pos, item in enumerate(items):
        ts = templates.by_relation.get(item.prop)
        if ts is None:
            raise E5DataError(f"no templates for relation {item.prop!r}")
        own = popqa_form(item)
        if own in ts:
            raise E5DataError(f"template for {item.prop!r} equals PopQA's own wording {own!r}")
        k = pos % PER_RELATION
        rows.append({"id": item.id, "original": item.question, "question": ts[k].replace(PLACEHOLDER, item.subj),
                     "template": k, "prop": item.prop, "aliases": list(item.aliases)})
    return rows
