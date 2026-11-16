import hashlib
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from ko import models as M                                              # noqa: E402


def lock_for(tmp_path, content=b"weights", file="Tiny-IQ4_XS.gguf"):
    e = {"id": "tiny", "repo": "x/Tiny", "file": file, "sha256": hashlib.sha256(content).hexdigest(),
         "size": len(content), "quant": "IQ4_XS", "lineage": "test"}
    return [e]


def test_real_lock_is_well_formed():
    lock = M.load_lock()
    ids = [e["id"] for e in lock]
    assert len(ids) == len(set(ids))
    assert all(len(e["sha256"]) == 64 for e in lock)
    assert {"qwen3.5-9b-iq4_xs", "qwen3.5-27b-iq4_xs", "qwen3.8-27b-iq4_xs-smaller"} <= set(ids)
    assert next(e for e in lock if e["id"] == "qwen3.8-9b-distill-q8_0")["lineage"].endswith("(inferred)")


def test_accepts_a_file_whose_hash_matches(tmp_path):
    (tmp_path / "Tiny-IQ4_XS.gguf").write_bytes(b"weights")
    got = M.verify("/some/where/Tiny-IQ4_XS.gguf", lock=lock_for(tmp_path), models_dir=tmp_path,
                   cache_path=tmp_path / "c.json")
    assert got["id"] == "tiny" and got["verified"] == "sha256"


def test_refuses_a_file_with_the_right_name_and_the_wrong_bytes(tmp_path):
    (tmp_path / "Tiny-IQ4_XS.gguf").write_bytes(b"a distill wearing the name")
    with pytest.raises(M.ModelRefused, match="not the one"):
        M.verify("Tiny-IQ4_XS.gguf", lock=lock_for(tmp_path), models_dir=tmp_path, cache_path=None)


def test_refuses_a_file_not_in_the_lock(tmp_path):
    with pytest.raises(M.ModelRefused, match="not in models.lock"):
        M.verify("Other.gguf", lock=lock_for(tmp_path), models_dir=tmp_path, cache_path=None)


def test_unreachable_file_is_refused_unless_name_only(tmp_path):
    lock = lock_for(tmp_path)
    with pytest.raises(M.ModelRefused, match="cannot be checked"):
        M.verify("Tiny-IQ4_XS.gguf", lock=lock, models_dir=tmp_path, cache_path=None)
    got = M.verify("Tiny-IQ4_XS.gguf", lock=lock, models_dir=tmp_path, name_only=True, cache_path=None)
    assert got["verified"] == "name-only"


def test_expect_pins_the_lock_id(tmp_path):
    (tmp_path / "Tiny-IQ4_XS.gguf").write_bytes(b"weights")
    with pytest.raises(M.ModelRefused, match="expects"):
        M.verify("Tiny-IQ4_XS.gguf", lock=lock_for(tmp_path), models_dir=tmp_path, expect="other",
                 cache_path=None)


def test_hash_is_cached_by_size_and_mtime(tmp_path):
    f = tmp_path / "m.gguf"
    f.write_bytes(b"abc")
    c = tmp_path / "c.json"
    first = M.file_sha256(f, c)
    assert json.loads(c.read_text()) and M.file_sha256(f, c) == first
    f.write_bytes(b"abcd")
    assert M.file_sha256(f, c) == hashlib.sha256(b"abcd").hexdigest()
