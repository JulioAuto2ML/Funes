import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import stats_holm as H   # noqa: E402


def test_holm_known_values():
    adj = H.holm({"a": 0.01, "b": 0.04, "c": 0.03})
    assert abs(adj["a"] - 0.03) < 1e-12 and abs(adj["c"] - 0.06) < 1e-12 and abs(adj["b"] - 0.06) < 1e-12


def test_holm_monotone_and_capped():
    adj = H.holm({"a": 0.5, "b": 0.9, "c": 1.0})
    assert adj["a"] <= adj["b"] <= adj["c"] <= 1.0


def test_sign_p():
    assert H.sign_p(0, 0, True) == 1.0
    assert abs(H.sign_p(1, 9, True) - 2 * 11 / 1024) < 1e-12      # 2*(C(10,0)+C(10,1))/2^10
    assert abs(H.sign_p(10, 0, False) - 1 / 1024) < 1e-12


def test_h5_default_entry_needs_both_parts():
    assert H.h5_default_entry(None, 1e-20) == 1.0          # H5a untestable -> H5 does not count
    assert H.h5_default_entry(0.135, 1e-24) == 0.135       # equivalence not shown dominates
    assert H.h5_default_entry(1e-4, 0.02) == 0.02
