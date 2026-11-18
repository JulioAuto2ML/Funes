import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import gguf_template as G   # noqa: E402


def _s(x):
    b = x.encode()
    return struct.pack("<Q", len(b)) + b


def _file(tmp_path, with_template=True):
    kv = _s("general.name") + struct.pack("<I", 8) + _s("t")
    kv += _s("tokenizer.ggml.tokens") + struct.pack("<I", 9) + struct.pack("<I", 8) + struct.pack("<Q", 3) + _s("a") + _s("bb") + _s("c")
    kv += _s("some.int") + struct.pack("<I", 4) + struct.pack("<I", 7)
    n = 3
    if with_template:
        kv += _s("tokenizer.chat_template") + struct.pack("<I", 8) + _s("{{ messages }}")
        n = 4
    data = b"GGUF" + struct.pack("<I", 3) + struct.pack("<Q", 0) + struct.pack("<Q", n) + kv + b"TENSORDATA" * 1000
    p = tmp_path / "m.gguf"
    p.write_bytes(data)
    return p, len(data) - 10000


def test_reads_the_template_skipping_arrays_and_stops_before_the_tensors(tmp_path):
    p, header_end = _file(tmp_path)
    ver, tpl, pos = G.chat_template(str(p))
    assert (ver, tpl, pos) == (3, "{{ messages }}", header_end)


def test_a_file_without_a_template_says_none(tmp_path):
    p, _ = _file(tmp_path, with_template=False)
    assert G.chat_template(str(p))[1] is None


def test_not_a_gguf_is_an_error(tmp_path):
    p = tmp_path / "x.gguf"
    p.write_bytes(b"NOPE" + b"0" * 20)
    try:
        G.chat_template(str(p))
        assert False
    except ValueError:
        pass
