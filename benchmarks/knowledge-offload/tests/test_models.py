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


# ---- E5: LoRA adapters (models.lock "adapters"; the gate that nothing unlocked is applied) -------------------------------
def adapter_lock(tmp_path, content=b"lora", base="tiny", file="L1_B_2B.gguf"):
    (tmp_path / file).write_bytes(content)
    return [{"id": "l1-b-2b", "file": file, "sha256": hashlib.sha256(content).hexdigest(), "base": base, "arm": "L1",
             "source_sha256": "0" * 64}]


def served(file="L1_B_2B.gguf", scale=1.0):
    return [{"id": 0, "path": f"/x/gguf/{file}", "scale": scale}]


def test_adapter_applied_and_expected_is_accepted(tmp_path):
    got = M.verify_adapters(served(), base_id="tiny", adapters=adapter_lock(tmp_path), adapters_dir=tmp_path,
                            expect="l1-b-2b", cache_path=None)
    assert [g["id"] for g in got] == ["l1-b-2b"] and got[0]["verified"] == "sha256"


def test_no_adapter_applied_is_the_plain_base(tmp_path):
    for active in ([], served(scale=0.0)):                     # none loaded, or loaded but switched off
        assert M.verify_adapters(active, base_id="tiny", adapters=[], adapters_dir=tmp_path) == []


def test_an_applied_adapter_without_expect_is_refused(tmp_path):
    with pytest.raises(M.ModelRefused, match="does not say it expects"):
        M.verify_adapters(served(), base_id="tiny", adapters=adapter_lock(tmp_path), adapters_dir=tmp_path, cache_path=None)


def test_expect_with_nothing_applied_is_refused(tmp_path):
    with pytest.raises(M.ModelRefused, match="no adapter applied"):
        M.verify_adapters([], base_id="tiny", adapters=adapter_lock(tmp_path), adapters_dir=tmp_path, expect="l1-b-2b")


def test_unlocked_adapter_wrong_bytes_wrong_base_and_odd_scale_are_refused(tmp_path):
    lock = adapter_lock(tmp_path)
    with pytest.raises(M.ModelRefused, match="not in models.lock"):
        M.verify_adapters(served("other.gguf"), base_id="tiny", adapters=lock, adapters_dir=tmp_path, expect="l1-b-2b")
    (tmp_path / "L1_B_2B.gguf").write_bytes(b"retrained, same name")
    with pytest.raises(M.ModelRefused, match="not the adapter"):
        M.verify_adapters(served(), base_id="tiny", adapters=lock, adapters_dir=tmp_path, expect="l1-b-2b", cache_path=None)
    lock = adapter_lock(tmp_path)
    with pytest.raises(M.ModelRefused, match="built for tiny"):
        M.verify_adapters(served(), base_id="big", adapters=lock, adapters_dir=tmp_path, expect="l1-b-2b", cache_path=None)
    with pytest.raises(M.ModelRefused, match="scale 0.5"):
        M.verify_adapters(served(scale=0.5), base_id="tiny", adapters=lock, adapters_dir=tmp_path, expect="l1-b-2b", cache_path=None)


def test_unreadable_lora_endpoint_is_refused(tmp_path):
    with pytest.raises(M.ModelRefused, match="could not be read"):
        M.verify_adapters(None, base_id="tiny", adapters=[], adapters_dir=tmp_path)


def test_real_lock_adapters_are_well_formed():
    ads = M.load_adapters()
    ids = [e["id"] for e in ads]
    assert len(ids) == len(set(ids))
    base_ids = {e["id"] for e in M.load_lock()}
    assert all(len(e["sha256"]) == 64 and e["base"] in base_ids for e in ads)
