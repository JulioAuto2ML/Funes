"""The `funes` condition: the question through /api/chat of a scratch Funes.

Never yoda's `data/memory.db`: the instance gets its own FUNES_DB and
FUNES_DATA_DIR in a fresh temp directory, a throwaway account, and the
background passes that would spend the GPU on a big pool switched off. The
model and the embedder are yoda's llama-servers (FUNES_LLM_URL/EMBED_URL);
the C++ agent loop itself runs wherever the binary is, which is not inference.
"""
import http.cookiejar
import json
import os
import secrets
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

from .conditions import Answer, ConditionUnavailable
from .llm import Usage

REPO = Path(__file__).resolve().parents[3]


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class FunesInstance:
    def __init__(self, llm_url: str, embed_url: str, binary: Path | None = None,
                 pool: Path | None = None):
        """`pool`: a directory made by `build_pool` — its memory.db is copied into
        the scratch dir and its throwaway account is logged into, not created."""
        self.pool = Path(pool) if pool else None
        self.binary = Path(binary or REPO / "bin" / "funes")
        if not self.binary.exists():
            raise ConditionUnavailable(f"{self.binary} not built (cmake --build build)")
        self.llm_url, self.embed_url = llm_url, embed_url
        self.scratch = Path(tempfile.mkdtemp(prefix="ko_funes_"))
        self.port = _free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.proc = None
        self.log = self.scratch / "funes.log"
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))
        self.username = "ko_" + secrets.token_hex(3)
        self._password = secrets.token_urlsafe(16)
        if self.pool:
            acct = json.loads((self.pool / "account.json").read_text())
            self.username, self._password = acct["username"], acct["password"]

    # -- lifecycle --------------------------------------------------------
    def __enter__(self):
        db = self.scratch / "memory.db"
        assert db.resolve().is_relative_to(Path(tempfile.gettempdir()).resolve()) or \
            not db.resolve().is_relative_to(REPO / "data"), "refusing a non-scratch FUNES_DB"
        prod = REPO / "data" / "memory.db"
        self._prod_mtime = prod.stat().st_mtime if prod.exists() else None
        if self.pool:
            for f in self.pool.glob("memory.db*"):        # -wal / -shm travel with the db
                shutil.copy2(f, self.scratch / f.name)
        env = {k: v for k, v in os.environ.items() if k in ("PATH", "HOME", "LANG", "LC_ALL")}
        env.update({
            "FUNES_DB": str(db), "FUNES_DATA_DIR": str(self.scratch / "data"),
            "FUNES_WORKSPACE_DIR": str(self.scratch / "ws"),
            "FUNES_AGENTS_DIR": str(REPO / "agents"), "FUNES_PORT": str(self.port),
            "FUNES_HOST": "127.0.0.1", "FUNES_LLM_URL": self.llm_url,
            "FUNES_EMBED_URL": self.embed_url,
            # An exchange must not become a memory the next question can recall.
            "FUNES_AUTO_MEMORY": "off", "FUNES_CONSOLIDATE": "off",
            "FUNES_LINK_BACKFILL": "off", "FUNES_PROBES": "off", "FUNES_GUARDIAN": "off"})
        self.proc = subprocess.Popen([str(self.binary)], cwd=REPO, env=env,
                                     stdout=open(self.log, "w"), stderr=subprocess.STDOUT)
        for _ in range(100):
            time.sleep(0.3)
            if self.proc.poll() is not None:
                raise ConditionUnavailable(f"funes exited at startup; see {self.log}")
            try:
                self._req("/api/auth/status")
                break
            except Exception:
                continue
        else:
            raise ConditionUnavailable(f"funes did not come up; see {self.log}")
        if not self.pool:
            self._req("/api/auth/bootstrap", {"username": self.username, "password": self._password,
                                              "display_name": "KO bench"})
        self._req("/api/login", {"username": self.username, "password": self._password})
        return self

    def _stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(30)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def __exit__(self, *exc):
        self._stop()
        prod = REPO / "data" / "memory.db"
        now = prod.stat().st_mtime if prod.exists() else None
        shutil.rmtree(self.scratch, ignore_errors=True)
        if now != self._prod_mtime:
            raise RuntimeError("repo data/memory.db changed during the funes condition")

    # -- http -------------------------------------------------------------
    def _req(self, path: str, body: dict | None = None, timeout: float = 600):
        req = urllib.request.Request(
            self.base + path, data=json.dumps(body).encode() if body is not None else None,
            headers={"Content-Type": "application/json"})
        with self.opener.open(req, timeout=timeout) as r:
            return r.read().decode()

    def load_facts(self, texts, log=None) -> int:
        n = 0
        for t in texts:
            self._req("/api/memories", {"text": t})
            n += 1
            if log and n % 1000 == 0:
                log(f"  pool {n}/{len(texts)}")
        return n

    def save_pool(self, dest: Path) -> Path:
        """Stop the server (so the WAL is folded into the db) and keep its
        memory.db and account. Ends this instance: build a pool, then open
        each run on a copy of it."""
        self._stop()
        dest = Path(dest)
        dest.mkdir(parents=True, exist_ok=True)
        for f in self.scratch.glob("memory.db*"):
            shutil.copy2(f, dest / f.name)
        (dest / "account.json").write_text(json.dumps(
            {"username": self.username, "password": self._password, "embed_url": self.embed_url}))
        (dest / "account.json").chmod(0o600)
        return dest

    def ask(self, question: str) -> Answer:
        t0 = time.monotonic()
        raw = self._req("/api/chat", {"message": question, "session": "q" + secrets.token_hex(6)})
        wall = (time.monotonic() - t0) * 1000
        tools, text, usage, err, event = [], "", Usage(wall_ms=wall, calls=1), None, ""
        context_used = None
        for line in raw.splitlines():
            if line.startswith("event:"):
                event = line[6:].strip()
            elif line.startswith("data:") and event:
                try:
                    d = json.loads(line[5:])
                except json.JSONDecodeError:
                    continue
                if event == "tool_call":
                    tools.append(d.get("name", ""))
                elif event == "usage":
                    # The SSE reports context fill ("used"/"limit"), not billed
                    # tokens: a funes answer's cost is wall time only.
                    context_used = d.get("used")
                elif event == "done":
                    text = d.get("text", "")
                elif event == "error":
                    err = d.get("error") or d.get("message") or str(d)
        tainted = any(t in ("web_search", "web_fetch", "delegate_to_agent") for t in tools)
        return Answer(text=text.strip(), usage=usage, tools=tools, tainted=tainted, error=err,
                      extra={"context_used": context_used})
