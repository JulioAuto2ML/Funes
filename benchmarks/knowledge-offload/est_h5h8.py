#!/usr/bin/env python3
"""Run-time estimate for the default-prompt `tool` / `tool+contract` cells (D12/D13/D15(1)), from the cells already measured.

  python3 est_h5h8.py        # -> prints the table; numbers marked [m] are measured, [x] extrapolated

Measured here: per-cell wall_s of the E1 `closed`, `tool`, `tool+contract` (search_first) and ablated cells; swap overhead per
size (block time in results/queue.log minus the sum of its cells' wall_s). Extrapolated, and said so: the default prompt changes
how many calls a model makes (E0, a different model, 9B distill: default `tool` 1.006 calls/answer, lookup .006; default
`tool+contract` 2.994 calls/answer), so each cell is given as a LOW and a HIGH, not one number.
  tool default   LOW  = 1.795 x closed  (E0 ratio tool/closed, 409.5/228.2 s: the tool schema is in the prompt) [x]
                 HIGH = the measured search_first `tool` time (the model looks up on every question) [m]
  contract default LOW = measured search_first `tool+contract` time [m]
                   HIGH = LOW x 3 / calls-per-answer at search_first, never below LOW (model skips the tool, is nudged to 3 calls) [x]
  ablated (200 q)  = 0.21 x the main cell (measured 0.206-0.209 at 27B and 9B) [x for the default prompt]
"""
import glob
import json
import re
import datetime as dt
import statistics
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
R = HERE / "results"
E0_RATIO = 409.5 / 228.2
ABL = 0.21
JOBS = ["qwen3.5-9b-iq4_xs", "qwen3.5-4b-iq4_xs", "qwen3.5-2b-iq4_xs", "qwen3.5-0.8b-iq4_xs", "qwen3.5-27b-iq4_xs",
        "qwen3.5-9b-q8_0", "qwen3.5-4b-q8_0", "qwen3.5-2b-q8_0", "qwen3.5-0.8b-q8_0"]


def cells():
    out = {}
    for f in glob.glob(str(R / "popqa-stratified1u-n1000-*-r0.json")):
        r = json.loads(Path(f).read_text())
        a = [x for x in r["answers"] if x["pass"] == 1]
        out[(r["model_tag"], r["condition"], r["prompt_variant"])] = (r["wall_s"], statistics.mean(x["usage"]["calls"] for x in a))
    return out


def overhead():
    """Per block: queue.log start-to-rc=0 minus the sum of the tag's cell wall_s (main + ablated + web)."""
    starts, ends = {}, {}
    for line in (R / "queue.log").read_text().splitlines():
        ts, _, msg = line.partition(" ")
        if m := re.match(r"start (\S+) try", msg):
            starts[m.group(1)] = dt.datetime.fromisoformat(ts)
        if m := re.match(r"(\S+) rc=0", msg):
            ends[m.group(1)] = dt.datetime.fromisoformat(ts)
    wall = Counter()
    for f in glob.glob(str(R / "popqa-stratified1u*-r0.json")):
        r = json.loads(Path(f).read_text())
        wall[r["model_tag"]] += r["wall_s"]
    return {t: (ends[t] - starts[t]).total_seconds() - wall[t] for t in starts if t in ends}


def estimate():
    c, oh = cells(), overhead()
    rows = []
    for t in JOBS:
        closed = c[(t, "closed", "default")][0]
        sf_t, sf_tc = c[(t, "tool", "search_first")], c[(t, "tool+contract", "search_first")]
        tool_lo, tool_hi = min(E0_RATIO * closed, sf_t[0]), sf_t[0]
        tc_lo = sf_tc[0]
        tc_hi = max(tc_lo, tc_lo * 3 / sf_tc[1])
        lo, hi = (tool_lo + tc_lo) * (1 + ABL), (tool_hi + tc_hi) * (1 + ABL)
        rows.append((t, closed, sf_t, sf_tc, tool_lo, tool_hi, tc_lo, tc_hi, lo, hi, oh.get(t)))
    return rows, oh


def main():
    rows, oh = estimate()
    swap = statistics.mean(oh.values())
    print("swap + restore overhead per block, measured (queue.log minus cell wall_s): " +
          ", ".join(f"{t.split('qwen3.5-')[1]} {v:.0f}s" for t, v in sorted(oh.items())) + f"; max {max(oh.values()):.0f}s [m]")
    print(f"{'model file':<22}{'closed [m]':>11}{'sf tool [m]':>12}{'sf tc [m]':>10}{'sf calls t/tc':>14}{'tool dflt lo-hi [x]':>21}{'tc dflt lo-hi [x]':>20}{'block lo-hi (incl. ablated+swap)':>34}")
    tl = th = 0
    for t, closed, sft, sftc, tlo, thi, clo, chi, lo, hi, _ in rows:
        blo, bhi = lo + max(oh.values()), hi + max(oh.values())
        tl += blo
        th += bhi
        print(f"{t:<22}{closed:>11.0f}{sft[0]:>12.0f}{sftc[0]:>10.0f}{sft[1]:>8.2f}/{sftc[1]:<5.2f}{tlo:>11.0f}-{thi:<9.0f}{clo:>10.0f}-{chi:<9.0f}{blo / 60:>14.0f}-{bhi / 60:<8.0f} min")
    print(f"TOTAL 9 model files: {tl / 3600:.1f} h (low) to {th / 3600:.1f} h (high)")
    return rows


def minutes(job: str) -> int:
    """Upper-bound block time in minutes (HIGH estimate incl. ablated + the largest measured swap overhead), rounded up to 5."""
    rows, oh = estimate()
    hi = next(r[9] for r in rows if r[0] == job) + max(oh.values())
    return int(-(-hi // 300) * 5)


if __name__ == "__main__":
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == "minutes":
        print(minutes(sys.argv[2]))
    else:
        main()
