"""The three default-style prompt variants (Opus, notebook "step 2 of the 9B plan"): the code must carry
the approved text byte for byte, apply to `tool` only, and change nothing else."""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

import pytest

from ko import conditions as C

NOTEBOOK = (HERE / "NOTEBOOK.md").read_text()
NEW = ("tool_first", "tool_free", "tool_describe")


def approved(name):
    m = re.search(r"^\d\. `%s`[^:\n]*:\s*`([^`\n]+)`" % name, NOTEBOOK, re.M)
    assert m, f"{name} not found in the notebook entry"
    return m.group(1)


class Ctx:
    def __init__(self, variant):
        self.variant = variant


@pytest.mark.parametrize("name", NEW)
def test_text_is_the_approved_text_byte_for_byte(name):
    assert C.system_for(Ctx(name), "lookup") == approved(name)


def test_the_baseline_text_is_untouched():
    assert C.system_for(Ctx("default"), "lookup") == (
        "You answer factual questions. Reply with only the answer: a short phrase, with no explanation "
        "and no full sentence. Use the tools you are given when they help.")


@pytest.mark.parametrize("name", NEW)
def test_variants_are_registered_and_apply_to_tool_only(name):
    assert name in C.VARIANTS
    assert C.VARIANT_APPLIES[name] == ("tool",)
    assert C.variant_for("tool", name) == name
    for other in ("closed", "oracle", "store", "tool+contract", "web", "web+cache"):
        assert C.variant_for(other, name) == "default"


def test_old_variants_still_resolve():
    for v in ("default", "search_first", "search_first2", "abstain_ok", "abstain_default"):
        assert v in C.VARIANTS
    assert C.system_for(Ctx("search_first"), "lookup").startswith("You answer factual questions about specific")


def test_the_three_texts_differ_from_each_other_and_from_default():
    texts = {C.system_for(Ctx(v), "lookup") for v in ("default",) + NEW}
    assert len(texts) == 4


def test_tool_free_is_the_baseline_without_its_format_sentence():
    assert C.system_for(Ctx("tool_free"), "lookup") == "You answer factual questions. Use the tools you are given when they help."


def test_tool_first_is_the_same_words_in_another_order():
    a = sorted(C.system_for(Ctx("default"), "x").replace(".", "").replace(":", "").split())
    b = sorted(C.system_for(Ctx("tool_first"), "x").replace(".", "").replace(":", "").split())
    assert a == b
