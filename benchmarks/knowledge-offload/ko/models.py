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


def load_adapters(path: Path = LOCK_PATH) -> list[dict]:
    """LoRA adapters the benchmark may serve (E5): the `adapters` list of models.lock. Each names the lock model it was
    trained for and applied on (`base`), the GGUF's sha256, and the safetensors it was converted from."""
    entries = json.loads(Path(path).read_text()).get("adapters", [])
    for e in entries:
        for k in ("id", "file", "sha256", "base", "arm", "source_sha256"):
            if k not in e:
                raise ValueError(f"{path}: adapter {e.get('id', '?')} lacks {k!r}")
    return entries


def verify_adapters(active: list[dict] | None, *, base_id: str, adapters: list[dict] | None = None,
                    adapters_dir: Path | None = None, expect: str | None = None,
                    cache_path: Path | None = CACHE_PATH) -> list[dict]:
    """The lock entries of the adapters a server has applied (scale != 0), or ModelRefused.

    `active` is llama-server's `/lora-adapters` (None = could not be read: refused, since a run must know that no
    unlocked adapter is on). An applied adapter is accepted only when `expect` names it, its file's sha256 is the lock's,
    and it was built for the served base model. Without `expect` an applied adapter is refused: a base cell must not run
    on whatever a server happens to have loaded. A loaded adapter at scale 0 is not applied and is ignored; `expect` with
    nothing applied is refused (the run would be labelled with an adapter it did not use)."""
    if active is None:
        raise ModelRefused("the server's /lora-adapters could not be read, so it cannot be shown that no unlocked adapter is applied")
    adapters = load_adapters() if adapters is None else adapters
    applied = [a for a in active if float(a.get("scale", 0)) != 0.0]
    if not applied:
        if expect:
            raise ModelRefused(f"run expects adapter {expect}, but the server has no adapter applied (scale 0 or none loaded)")
        return []
    out = []
    for a in applied:
        base = Path(a.get("path", "")).name
        named = [e for e in adapters if e["file"] == base]
        if not named:
            raise ModelRefused(f"an adapter {base!r} is applied (scale {a.get('scale')}) and is not in models.lock")
        path = Path(adapters_dir or "") / base
        if not path.exists():
            raise ModelRefused(f"{path} is not readable from here, so the applied adapter's hash cannot be checked")
        got = file_sha256(path, cache_path)
        match = [e for e in named if e["sha256"] == got]
        if not match:
            raise ModelRefused(f"{path} has sha256 {got}, models.lock says {named[0]['sha256']}: not the adapter the lock describes")
        e = match[0]
        if e["base"] != base_id and base_id not in e.get("transfer_bases", []):
            raise ModelRefused(f"adapter {e['id']} was built for {e['base']}, the server is serving {base_id}")
        if float(a["scale"]) != 1.0:
            raise ModelRefused(f"adapter {e['id']} is applied at scale {a['scale']}; the benchmark runs adapters at 1.0 only")
        out.append({**e, "scale": float(a["scale"]), "verified": "sha256", "transfer": e["base"] != base_id})
    if expect is None:
        raise ModelRefused(f"adapter(s) {[e['id'] for e in out]} are applied but the run does not say it expects one (--expect-adapter)")
    if [e["id"] for e in out] != [expect]:
        raise ModelRefused(f"run expects adapter {expect}, the server applies {[e['id'] for e in out]}")
    return out
