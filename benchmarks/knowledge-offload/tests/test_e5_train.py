"""E5 training script, the parts that need no torch: example building from the checked mask, padding, the batch schedule."""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

import pytest

import e5_check_template as C
import e5_train as T


def stub(monkeypatch, ids, train, straddling=()):
    monkeypatch.setattr(C, "trained_spans", lambda tpl, row: ([], "full"))
    monkeypatch.setattr(C, "masked_tokens", lambda tok, full, spans: {"ids": ids, "train": train, "straddling_tokens": list(straddling)})


def test_labels_are_ids_where_trained_and_ignore_elsewhere(monkeypatch):
    stub(monkeypatch, [5, 6, 7, 8], [False, False, True, True])
    ex = T.build_example(None, None, {"id": "r"}, 16)
    assert ex == {"input_ids": [5, 6, 7, 8], "labels": [T.IGNORE, T.IGNORE, 7, 8]}


def test_too_long_is_dropped_not_truncated(monkeypatch):
    stub(monkeypatch, list(range(10)), [True] * 10)
    assert T.build_example(None, None, {"id": "r"}, 9) is None


def test_row_with_nothing_to_train_is_dropped(monkeypatch):
    stub(monkeypatch, [1, 2], [False, False])
    assert T.build_example(None, None, {"id": "r"}, 16) is None


def test_a_token_straddling_a_span_edge_is_an_error(monkeypatch):
    stub(monkeypatch, [1, 2], [True, True], straddling=["x"])
    with pytest.raises(ValueError):
        T.build_example(None, None, {"id": "r"}, 16)


def test_collate_pads_right_with_attention_zero_and_ignore():
    c = T.collate([{"input_ids": [1, 2, 3], "labels": [T.IGNORE, 2, 3]}, {"input_ids": [4], "labels": [4]}], pad_id=9)
    assert c["input_ids"] == [[1, 2, 3], [4, 9, 9]]
    assert c["labels"] == [[T.IGNORE, 2, 3], [4, T.IGNORE, T.IGNORE]]
    assert c["attention_mask"] == [[1, 1, 1], [1, 0, 0]]


def test_schedule_is_deterministic_and_covers_each_example_once_per_epoch():
    ex = [{"input_ids": [i], "labels": [i]} for i in range(10)]
    a, b = T.batches(ex, 4, 2, seed=3), T.batches(ex, 4, 2, seed=3)
    assert a == b
    assert len(a) == 6                                   # 3 batches per epoch (4, 4, 2), two epochs
    for e in range(2):
        seen = sorted(x["input_ids"][0] for bt in a[3 * e:3 * e + 3] for x in bt)
        assert seen == list(range(10))
    assert a != T.batches(ex, 4, 2, seed=4)
