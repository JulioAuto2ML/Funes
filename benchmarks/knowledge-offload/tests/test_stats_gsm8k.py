"""H2 statistics on synthetic cells: a store that follows the size curve, one that flattens it, a flat closed curve."""
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

import stats_gsm8k as S

SIZES = ["0.8b", "2b", "4b", "9b", "27b"]


def write(d, accs_closed, accs_store, n=500, quant="iq4_xs"):
    rng = np.random.default_rng(0)
    for cond, accs in (("closed", accs_closed), ("store", accs_store)):
        for s, a in zip(SIZES, accs):
            hit = set(rng.permutation(n)[: int(round(a * n))])
            rows = [{"id": f"p{i}", "correct": i in hit, "correct_strict": i in hit} for i in range(n)]
            summ = {"n": n, "acc": len(hit) / n, "acc_strict": len(hit) / n, "modes": {"strict": 1.0, "fallback": 0.0, "none": 0.0},
                    "format_dominated": False, "at_limit": 0.0, "completion_tokens_mean": 200.0, "completion_tokens_max": 400}
            (d / f"gsm8k-n500-s1-qwen3.5-{s}-{quant}-{cond}-r0.json").write_text(json.dumps(
                {"model_tag": f"qwen3.5-{s}-{quant}", "condition": cond, "pilot": False, "wall_s": 10.0,
                 "answers": rows, "summary": summ}))


def test_store_follows_curve_confirms(tmp_path):
    curve = [0.1, 0.25, 0.45, 0.65, 0.85]
    write(tmp_path, curve, curve)
    r = S.h2(S.load(tmp_path), "iq4_xs", nb=2000)
    assert r["evaluable"] and r["verdict"].startswith("H2 confirmed") and 0.8 < r["rho"] < 1.2


def test_store_flat_refutes(tmp_path):
    write(tmp_path, [0.1, 0.25, 0.45, 0.65, 0.85], [0.4, 0.4, 0.4, 0.4, 0.4])
    r = S.h2(S.load(tmp_path), "iq4_xs", nb=2000)
    assert r["verdict"].startswith("H2 refuted") and r["p"] > 0.5


def test_flat_closed_is_not_evaluable(tmp_path):
    write(tmp_path, [0.5, 0.5, 0.5, 0.5, 0.5], [0.5, 0.5, 0.5, 0.5, 0.5])
    r = S.h2(S.load(tmp_path), "iq4_xs", nb=2000)
    assert not r["evaluable"] and "not evaluable" in r["verdict"]


def test_fewer_than_three_sizes_is_none(tmp_path):
    write(tmp_path, [0.1, 0.2, 0.3, 0.4, 0.5], [0.1, 0.2, 0.3, 0.4, 0.5])
    for f in list(tmp_path.glob("*27b*")) + list(tmp_path.glob("*9b*")) + list(tmp_path.glob("*4b*")):
        f.unlink()
    assert S.h2(S.load(tmp_path), "iq4_xs", nb=100) is None


def test_build_writes_a_table(tmp_path):
    curve = [0.1, 0.25, 0.45, 0.65, 0.85]
    write(tmp_path, curve, curve)
    text = S.build(tmp_path)
    assert "H2, iq4_xs" in text and "Inputs (sha256)" in text and "q8_0" in text


def test_holm_h2_entry_is_one_without_cells_and_p_with_cells(tmp_path):
    import stats_holm as H
    assert H.h2_p(tmp_path, "iq4_xs") == 1.0
    curve = [0.1, 0.25, 0.45, 0.65, 0.85]
    write(tmp_path, curve, curve)
    p = H.h2_p(tmp_path, "iq4_xs")
    assert 0 < p < 0.05
    write(tmp_path, [0.5] * 5, [0.5] * 5)               # closed flat: not evaluable -> 1.0
    assert H.h2_p(tmp_path, "iq4_xs") == 1.0
