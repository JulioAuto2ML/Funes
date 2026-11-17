import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from ko import validity as V   # noqa: E402

WEB = {"condition": "web"}


def test_non_web_cells_are_never_flagged():
    assert V.status({"condition": "store"}, "x.json", {}) == ("ok", "")


def test_a_web_cell_with_counted_errors_is_invalid():
    st, why = V.status({**WEB, "search": {"errors": 3, "error_log": [{"status": 402, "text": "402: Insufficient credit"}]}}, "x.json", {})
    assert st == "INVALID" and "402" in why


def test_a_web_cell_without_the_counter_is_unchecked_not_valid():
    assert V.status({**WEB, "search": {"rate_limited": 0, "searches": 5}}, "x.json", {})[0] == "unchecked"


def test_a_payload_audit_decides_cells_that_predate_the_counter():
    notes = {"bad.json": {"failed_searches": 1661, "searches": 2086, "reason": "r"},
             "good.json": {"failed_searches": 0, "searches": 686, "reason": "r"}}
    assert V.status(WEB, "bad.json", notes)[0] == "INVALID"
    assert V.status(WEB, "good.json", notes) == ("ok", "")


def test_a_clean_counted_web_cell_is_ok():
    assert V.status({**WEB, "search": {"errors": 0}}, "x.json", {}) == ("ok", "")
