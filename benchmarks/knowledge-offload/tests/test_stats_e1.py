"""stats_e1 helpers: slope matches np.polyfit, bootstrap indices are reproducible and in range."""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import stats_e1 as S   # noqa: E402


def test_slope_matches_polyfit():
    x = np.log10([0.8, 2, 4, 9, 27])
    y = np.array([0.2, 0.25, 0.3, 0.36, 0.43])
    assert abs(S.slope(y, x) - np.polyfit(x, y, 1)[0]) < 1e-12
    assert np.allclose(S.slope(np.stack([y, 2 * y]), x), [np.polyfit(x, y, 1)[0], 2 * np.polyfit(x, y, 1)[0]])


def test_boot_idx_reproducible_and_bounded():
    a, b = S.boot_idx(50, nb=10, seed=3), S.boot_idx(50, nb=10, seed=3)
    assert (a == b).all() and a.min() >= 0 and a.max() < 50 and a.shape == (10, 50)


def test_pct_orders():
    lo, hi = S.pct(np.arange(100))
    assert lo < hi
