"""E5 LoRA gate: the comparison and the verdict (the HTTP parts need a server and are run on yoda)."""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

import e5_lora_gate as G

R = lambda c, calls=(): {"content": c, "calls": list(calls)}


def test_identical_replies_pass_scale_zero():
    base = [R("a"), R("", [("lookup", "{}")])]
    z = G.compare(base, [R("a"), R("", [("lookup", "{}")])])
    assert z["identical"] == 2 and z["differing"] == []


def test_a_single_difference_is_found_and_calls_are_counted():
    base = [R("a"), R("b")]
    o = G.compare(base, [R("a"), R("", [("lookup", "{}")])])
    assert o["differing"] == [1] and o["lookup_calls"] == 1


def test_verdict_needs_zero_identical_and_one_different():
    ok0, bad0 = {"identical": 3, "n": 3}, {"identical": 2, "n": 3}
    assert G.verdict(ok0, {"differing": [0]})
    assert not G.verdict(ok0, {"differing": []})       # adapter does nothing
    assert not G.verdict(bad0, {"differing": [0]})     # switched off but still changes replies
