import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import est_h5h8 as E   # noqa: E402


def test_estimate_has_every_job_with_low_not_above_high():
    rows, oh = E.estimate()
    assert [r[0] for r in rows] == E.JOBS and oh
    for r in rows:
        assert 0 < r[8] <= r[9]


def test_minutes_is_a_multiple_of_five_covering_the_high_estimate():
    rows, oh = E.estimate()
    for r in rows:
        m = E.minutes(r[0])
        assert m % 5 == 0 and m * 60 >= r[9] + max(oh.values())
