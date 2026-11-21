"""E5 dev eval: dev rows become harness Items without changing what pop_bucket and the scorer see."""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

import e5_dev_eval as D
from ko import popqa


def test_dev_rows_become_items_with_their_bucket(tmp_path):
    f = tmp_path / "dev.jsonl"
    f.write_text(json.dumps({"id": "7", "question": "Who is the author of X?", "subj": "X", "prop": "author",
                             "aliases": ["A B", "A"], "bucket": "1e3", "s_uri": "u"}) + "\n\n")
    [it] = D.dev_items(f)
    assert (it.id, it.obj, it.aliases) == ("7", "A B", ("A B", "A"))
    assert popqa.pop_bucket(it) == "1e3"


def test_by_bucket_counts_lookup_and_accuracy():
    rows = [{"pop_bucket": "1e1", "tools": ["lookup"], "contains": 1}, {"pop_bucket": "1e1", "tools": [], "contains": 0},
            {"pop_bucket": "1e2", "tools": ["lookup"], "contains": 0}]
    r = D.by_bucket(rows)
    assert r["1e1"] == {"n": 2, "lookup_rate": 0.5, "acc": 0.5}
    assert r["1e2"]["lookup_rate"] == 1.0
