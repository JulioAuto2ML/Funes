"""The `ablated` arm (H5b): the question's own fact is absent from the store, so the
only honest answers are an abstention or one from the weights; anything else is a
hallucination. This module says which.

Outcomes: `correct` (an alias is in the answer, whether from the weights or a
near-miss that happened to match), `abstained` (the answer says it does not know,
or is empty), `hallucinated` (an answer that is neither). The regex is checked
against 50 hand labels (`label_abstain.py`) before any number is reported.
"""
import re

from .scoring import normalize

ABSTAIN = re.compile(
    r"\b(i (do not|don'?t|dont|can'?t|cannot) (really )?(know|say|tell|determine|find|answer)|"
    r"i['\u2019]?m not (sure|certain)|not sure|unknown|unclear|no (stored )?(facts?|information|data|answer|record)|"
    r"(does|do|did) not (contain|say|mention|provide|specify|include)|(doesn|don|didn)['\u2019]?t (contain|say|mention|provide|specify|include)|"
    r"not (provided|specified|mentioned|available|found|stated|given|in the (facts|store|lookup|results))|"
    r"unable to|cannot be (determined|found)|insufficient)\b", re.I)


def abstains(prediction: str) -> bool:
    return not normalize(prediction) or bool(ABSTAIN.search(prediction or ""))


def outcome(prediction: str, contains: bool) -> str:
    """`contains`: the alias check from scoring.score. A correct answer that also
    says "I'm not sure" is correct: the model answered."""
    if contains:
        return "correct"
    return "abstained" if abstains(prediction) else "hallucinated"
