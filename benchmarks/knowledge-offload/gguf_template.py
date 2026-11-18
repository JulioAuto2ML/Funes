#!/usr/bin/env python3
"""Read-only: the `tokenizer.chat_template` string of GGUF files, and how many bytes of each file had to be read.

  python3 gguf_template.py FILE [FILE ...]

Reads only the header (metadata key/values precede the tensors); never the tensor data. Prints per file: header bytes read up to
and including the template, the file size, the template's sha256 and length. Standard library only, so it runs on yoda as is.
"""
import hashlib
import os
import struct
import sys

SCALAR = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}


def _read(f, fmt):
    n = struct.calcsize(fmt)
    b = f.read(n)
    if len(b) != n:
        raise EOFError
    return struct.unpack("<" + fmt, b)[0]


def _string(f):
    return f.read(_read(f, "Q")).decode("utf-8", "replace")


def _value(f, t, keep):
    if t in SCALAR:
        return _read(f, SCALAR[t])
    if t == 8:
        s = _string(f)
        return s
    if t == 9:
        et = _read(f, "I")
        n = _read(f, "Q")
        for _ in range(n):
            _value(f, et, False)
        return None
    raise ValueError(f"unknown GGUF value type {t}")


def chat_template(path):
    with open(path, "rb") as f:
        if f.read(4) != b"GGUF":
            raise ValueError("not a GGUF file")
        version = _read(f, "I")
        _read(f, "Q")
        n_kv = _read(f, "Q")
        for _ in range(n_kv):
            key = _string(f)
            t = _read(f, "I")
            v = _value(f, t, key == "tokenizer.chat_template")
            if key == "tokenizer.chat_template":
                return version, v, f.tell()
    return version, None, None


def main():
    for p in sys.argv[1:]:
        try:
            ver, tpl, pos = chat_template(p)
        except Exception as e:                                          # noqa: BLE001
            print(f"{os.path.basename(p)}\tERROR {type(e).__name__}: {e}")
            continue
        if tpl is None:
            print(f"{os.path.basename(p)}\tgguf v{ver}\tNO tokenizer.chat_template")
            continue
        print(f"{os.path.basename(p)}\tgguf v{ver}\tfile {os.path.getsize(p)} B\theader read {pos} B\t"
              f"template {len(tpl)} chars\tsha256 {hashlib.sha256(tpl.encode()).hexdigest()}")


if __name__ == "__main__":
    main()
