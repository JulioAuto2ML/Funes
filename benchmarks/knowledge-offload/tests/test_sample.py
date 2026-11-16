import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ko import popqa, runner                                            # noqa: E402
from ko.popqa import Item                                               # noqa: E402


def item(i, subj, prop, obj, pop=10):
    return Item(str(i), f"What is {subj}'s {prop}?", subj, prop, obj, (obj,), pop)


def table():
    rows = [item(1, "The Walk", "genre", "film"), item(2, "The Walk", "genre", "album"),   # homonym
            item(3, "Ada", "occupation", "mathematician"), item(4, "Ada", "country", "UK")]
    rows += [item(100 + k, f"S{k}", "occupation", f"o{k}", pop=10 ** (k % 4 + 1)) for k in range(80)]
    return rows


def test_unambiguous_drops_homonym_subjects_only():
    t = table()
    keep = {i.id for i in popqa.unambiguous(t)}
    assert "1" not in keep and "2" not in keep and {"3", "4"} <= keep and len(keep) == len(t) - 2


def test_two_rows_with_the_same_object_are_not_ambiguous():
    t = [item(1, "X", "genre", "film"), item(2, "X", "genre", "film")]
    assert len(popqa.unambiguous(t)) == 2


def test_stratified_sample_of_unambiguous_items_is_stratified_and_deterministic():
    t = table()
    pool = popqa.unambiguous(t)
    a = popqa.stratified_sample(pool, 40, 7)
    assert a == popqa.stratified_sample(pool, 40, 7) and a != popqa.stratified_sample(pool, 40, 8)
    assert not {"1", "2"} & {i.id for i in a}
    counts = Counter(popqa.pop_bucket(i) for i in a)
    assert max(counts.values()) - min(counts.values()) <= 1


def test_unambiguous_cells_do_not_overwrite_the_ambiguous_ones():
    kw = dict(sample="stratified", dataset_seed=1)
    a = runner.cell_path("m", 1000, "closed", 0, **kw)
    b = runner.cell_path("m", 1000, "closed", 0, unambiguous=True, **kw)
    assert a != b and "stratified1u" in b.name
    assert runner.payload_dir("m", "web", 0, 1, unambiguous=True, **kw) != runner.payload_dir("m", "web", 0, 1, **kw)
