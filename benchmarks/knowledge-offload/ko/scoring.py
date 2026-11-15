"""Exact / alias scoring for short answers (PopQA-style).

PopQA's own metric: the prediction is correct if any gold alias occurs in it
(Mallen et al. 2023). That is lenient to verbose answers, so we also report
`exact` (the whole normalised answer equals an alias) and grade on
`contains` but keep both in the per-question record.
"""
import re
import string
import unicodedata

_ARTICLES = re.compile(r"\b(a|an|the)\b")
_ABSTAIN = re.compile(
    r"\b(i don'?t know|i do not know|unknown|not sure|cannot (determine|answer)|"
    r"no (information|answer)|unable to (find|determine))\b", re.I)


def normalize(text: str) -> str:
    text = unicodedata.normalize("NFKD", text or "")
    text = "".join(c for c in text if not unicodedata.combining(c)).lower()
    text = "".join(c if c not in string.punctuation else " " for c in text)
    return " ".join(_ARTICLES.sub(" ", text).split())


def score(prediction: str, aliases: list[str]) -> dict:
    pred = normalize(prediction)
    norm = [normalize(a) for a in aliases if a and normalize(a)]
    exact = bool(pred) and pred in norm
    padded = f" {pred} "
    contains = bool(pred) and any(f" {a} " in padded for a in norm)
    return {"exact": exact, "contains": contains,
            "abstained": bool(_ABSTAIN.search(prediction or ""))}
