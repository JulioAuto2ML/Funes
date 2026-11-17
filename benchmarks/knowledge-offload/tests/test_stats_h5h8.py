import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import stats_h5h8 as H   # noqa: E402

RES = Path(__file__).resolve().parent.parent / "results"


def idx(n, nb=2000):
    return np.random.default_rng(0).integers(0, n, size=(nb, n))


def test_tost_identical_arms_is_equivalent():
    assert H.tost_p(np.zeros(1000), idx(1000)) < 1e-3


def test_tost_large_difference_is_not_equivalent():
    d = np.zeros(1000)
    d[:300] = 1.0
    assert H.tost_p(d, idx(1000)) > 0.99


def test_tost_difference_at_margin_is_inconclusive():
    d = np.zeros(1000)
    d[:20] = 1.0       # +.020 exactly at the margin
    assert 0.3 < H.tost_p(d, idx(1000)) < 0.7


def test_eligible_rule():
    assert H.eligible(0.875, 1.0) and not H.eligible(0.79, 1.0) and not H.eligible(0.9, 0.98)


def test_mh_pairs_known_values():
    assert H.mh_pairs(0, 0) == (1.0, 1.0, 0)
    ex, chi, n = H.mh_pairs(1, 9)
    assert n == 10 and abs(ex - 2 * 11 / 1024) < 1e-12
    assert abs(chi - 0.0114) < 1e-3          # chi2 = 6.4, 1 df


def test_analyse_on_real_cells():
    res = H.analyse(RES)
    assert res["n"] == 1000
    assert len(res["files"]) == 36
    r = res["h5a"][("4b", "iq4_xs")]
    assert r["eligible"] and r["diff"] > 0
    assert not res["h5a"][("9b", "iq4_xs")]["eligible"]
    a = res["abl"][("27b", "iq4_xs", H.ABL_TC)]
    assert abs(a["correct"][0] + a["abstained"][0] + a["hallucinated"][0] - 1) < 1e-9
    assert res["h8"][("iq4_xs", "tool")]["sizes"] == ["0.8b", "2b", "4b", "9b", "27b"]
    assert res["h8"][("q8_0", "tool")]["sizes"] == ["0.8b", "2b", "4b", "9b"]
