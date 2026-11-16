"""models.lock: which model files the benchmark may run on, and the gate that enforces it.

E0 ran on a file labelled "Qwen3.8 9B" that was a community distill; the check was
a substring of the file name. The gate here compares the sha256 of the file the
server was started from with the lock, and the lock's repo, lineage and quant are
copied into every result's meta.
"""
import hashlib
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
LOCK_PATH = HERE / "models.lock"
CACHE_PATH = HERE / "data" / "hash-cache.json"
DEFAULT_MODELS_DIR = Path(os.environ.get("FUNES_KO_MODELS_DIR", Path.home() / "llama.cpp" / "models"))


class ModelRefused(RuntimeError):
    pass


def load_lock(path: Path = LOCK_PATH) -> list[dict]:
    entries = json.loads(Path(path).read_text())["models"]
    for e in entries:
        for k in ("id", "repo", "file", "sha256", "size", "quant", "lineage"):
            if k not in e:
                raise ValueError(f"{path}: entry {e.get('id', '?')} lacks {k!r}")
    return entries


def file_sha256(path: Path, cache_path: Path | None = CACHE_PATH) -> str:
    """sha256 of a (large) file, remembered by path, size and mtime so a 15 GB
    model is read once, not on every run."""
    p = Path(path)
    st = p.stat()
    key = f"{p.resolve()}|{st.st_size}|{st.st_mtime_ns}"
    cache = {}
    if cache_path and Path(cache_path).exists():
        try:
            cache = json.loads(Path(cache_path).read_text())
        except json.JSONDecodeError:
            cache = {}
    if key in cache:
        return cache[key]
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
    cache[key] = h.hexdigest()
    if cache_path:
        Path(cache_path).parent.mkdir(parents=True, exist_ok=True)
        Path(cache_path).write_text(json.dumps(cache, indent=1))
    return cache[key]


def verify(served_file: str, *, lock: list[dict] | None = None, models_dir: Path = DEFAULT_MODELS_DIR,
           expect: str | None = None, name_only: bool = False,
           cache_path: Path | None = CACHE_PATH) -> dict:
    """The lock entry for the file being served, or ModelRefused.

    The file is looked up by base name in `models_dir` (the server's own directory;
    the harness normally runs on that host) and its hash compared with the lock.
    Not reachable from here: refused, unless `name_only` (recorded as such in
    the cell's meta: the weakest guarantee, for a tunnel).
    `expect` is an optional lock id the run insists on."""
    lock = load_lock() if lock is None else lock
    base = Path(served_file).name
    named = [e for e in lock if e["file"] == base]
    if not named:
        raise ModelRefused(f"{served_file!r} is not in models.lock (files: "
                           f"{', '.join(sorted({e['file'] for e in lock}))}). Add it with its repository hash or serve a locked file.")
    path = Path(models_dir) / base
    if path.exists():
        got = file_sha256(path, cache_path)
        match = [e for e in named if e["sha256"] == got]
        if not match:
            raise ModelRefused(f"{path} has sha256 {got}, models.lock says {named[0]['sha256']} for {base}: "
                               "the file is not the one the lock describes.")
        entry, how = match[0], "sha256"
    elif name_only:
        entry, how = named[0], "name-only"
    else:
        raise ModelRefused(f"{path} is not readable from here, so its hash cannot be checked. "
                           "Run on the host that holds the models, or pass --lock-name-only (recorded).")
    if expect and entry["id"] != expect:
        raise ModelRefused(f"serving {entry['id']}, run expects {expect}")
    return {**entry, "verified": how}
