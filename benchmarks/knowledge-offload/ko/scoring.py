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


def named_candidates(prediction: str, candidates) -> int:
    """How many of the candidate objects (each a tuple of aliases) the answer
    names. Longest alias first, and a match is struck out of the text once
    counted: "Georgia" inside "Georgia Institute of Technology" is not a second
    candidate, but a "Georgia" standing on its own still is."""
    padded = f" {normalize(prediction)} "
    pairs = sorted(((a, i) for i, al in enumerate(candidates)
                    for a in {normalize(x) for x in al if x} if a),
                   key=lambda t: -len(t[0]))
    named = set()
    for a, i in pairs:
        needle = f" {a} "
        if needle in padded:
            named.add(i)
            padded = padded.replace(needle, " ")
    return len(named)


def score(prediction: str, aliases: list[str], candidates=None) -> dict:
    """`candidates`: every object PopQA holds for this (subject, property), one
    alias tuple each — the homonym set. `hedge` is an answer naming two or more
    of them: `contains` is true for a list, which is not an answer."""
    pred = normalize(prediction)
    norm = [normalize(a) for a in aliases if a and normalize(a)]
    exact = bool(pred) and pred in norm
    padded = f" {pred} "
    contains = bool(pred) and any(f" {a} " in padded for a in norm)
    out = {"exact": exact, "contains": contains,
           "abstained": bool(_ABSTAIN.search(prediction or ""))}
    if candidates is not None:
        out["hedge"] = named_candidates(prediction, candidates) >= 2
    return out
