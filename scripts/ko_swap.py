#!/usr/bin/env python3
"""Swap yoda's :8080 model for a knowledge-offload size, and always swap it back.

  ko_swap.py check                      is a swap allowed now (cron windows)? exit 0/2
  ko_swap.py status                     what :8080 serves, next blocked window
  ko_swap.py run ID [--max-minutes N] -- CMD ...
                                        swap to the models.lock entry ID, wait healthy, run CMD
                                        (usually run.py), then restore qwen27b — on success, on
                                        failure, on ^C/SIGTERM, and when a cron window is about to open
  ko_swap.py start ID | restore         the two halves of `run`, for a by-hand session
  ko_swap.py loadtest ID                load a size (the 27B IQ4_XS, 14.98 GB, on 16 GB) with the
                                        benchmark flags and a long prompt; if f16 KV does not fit,
                                        retry with q4_0 and record that for EVERY size (~/.ko_kv)

Every size gets the same flags: f16 KV (unless the load test recorded q4_0, then q4_0 for all),
ctx 8192, one slot, flash attention, --jinja with thinking off, no projector, no prompt cache.
Production's own flags (q4_0 KV, --temp 0.8) are not used. The file's sha256 is checked against
benchmarks/knowledge-offload/models.lock BEFORE production is stopped.
It never starts a swap inside a window where Funes needs the GPU (scripts/yoda.crontab's image and
carousel jobs, plus the Funes cron jobs the crontab comments name: 06:30 news, the 09:00/10:30
issues) and ends a running one before the next window opens (the harness is resumable).
"""
import argparse
import datetime as dt
import fcntl
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
KO_DIR = HERE.parent / "benchmarks" / "knowledge-offload"
HOME = Path.home()
MODELS_DIR = HOME / "llama.cpp" / "models"
LLAMA_BIN = HOME / "llama.cpp" / "bin" / "llama-server"
LAUNCH = HOME / "launch_llama.sh"
LOG_DIR = HOME / "llama_logs"
SWAP_LOG = LOG_DIR / "model_swaps.log"
PID_FILE = Path("/tmp/llama_pids/llama_ko.pid")
PROD_PID = Path("/tmp/llama_pids/llama_qwen27b.pid")
KV_FILE = HOME / ".ko_kv"
CRONTAB = HERE / "yoda.crontab"
PORT = 8080
CTX = 8192
LEAD_MIN = 15            # a swap must be over this long before a window opens
PRODUCTION = "qwen27b"

# Funes cron jobs live in its database, not the crontab; the crontab's comments name when they run.
# (start "HH:MM", days, minutes the GPU is busy, what)
FUNES_JOBS = [("06:30", "*", 30, "news"), ("09:00", "1,3,5", 45, "AI Pulse issue"),
              ("10:30", "2,4,6", 45, "AI Pulse ES issue")]
# crontab entries that use the GPU: (command fragment, minutes busy)
CRON_GPU = [("run_images.sh", 25), ("weekly_carousel.py", 20)]


def days(spec: str) -> set[int]:
    """Cron day-of-week -> python weekday (Mon=0). Cron's 0 is Sunday."""
    if spec == "*":
        return set(range(7))
    return {(int(d) - 1) % 7 for d in spec.split(",")}


def blocked_windows(crontab_text: str) -> list[tuple[set, int, int, str]]:
    out = [(days(d), int(t[:2]) * 60 + int(t[3:]), n, w) for t, d, n, w in FUNES_JOBS]
    for line in crontab_text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = line.split(None, 5)
        if len(f) < 6 or not (f[0].isdigit() and f[1].isdigit()):
            continue
        for frag, minutes in CRON_GPU:
            if frag in f[5]:
                out.append((days(f[4]), int(f[1]) * 60 + int(f[0]), minutes, frag))
    return out


def blocking(now: dt.datetime, windows, lead: int = LEAD_MIN):
    """The window `now` is in, or within `lead` minutes of, else None."""
    m = now.hour * 60 + now.minute
    for dset, start, dur, what in windows:
        if now.weekday() in dset and start - lead <= m < start + dur:
            return what
    return None


def next_block(now: dt.datetime, windows, lead: int = LEAD_MIN, horizon_h: int = 36):
    """(datetime the next blocked stretch begins, what), scanning minute by minute."""
    t = now.replace(second=0, microsecond=0)
    for _ in range(horizon_h * 60):
        t += dt.timedelta(minutes=1)
        w = blocking(t, windows, lead)
        if w:
            return t, w
    return None


def kv_choice(path: Path = KV_FILE) -> str:
    try:
        v = path.read_text().strip()
    except OSError:
        return "f16"
    return v if v in ("f16", "q4_0") else "f16"


def server_args(model: Path, kv: str = "f16") -> list[str]:
    """One flag set for every size; only -m differs. q4_0 KV applies to all sizes or none."""
    a = [str(LLAMA_BIN), "-m", str(model), "--port", str(PORT), "--host", "127.0.0.1", "-t", "8", "-ngl", "99",
         "-fa", "on", "--ctx-size", str(CTX), "--parallel", "1", "--cache-ram", "0",
         "--jinja", "--reasoning-budget", "0", "--chat-template-kwargs", '{"enable_thinking":false}']
    if kv == "q4_0":
        a += ["-ctk", "q4_0", "-ctv", "q4_0"]
    return a


def log(msg: str) -> None:
    LOG_DIR.mkdir(exist_ok=True)
    with open(SWAP_LOG, "a") as f:
        f.write(f"{dt.datetime.now().isoformat(timespec='seconds')} ko-swap: {msg}\n")
    print(f"ko-swap: {msg}", flush=True)


def healthy(timeout: float = 2.0) -> bool:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/health", timeout=timeout) as r:
            return r.status == 200
    except Exception:
        return False


class Swap:
    """The side effects, behind methods a test replaces."""

    def __init__(self, now=dt.datetime.now, sleep=time.sleep, healthy=healthy, spawn=subprocess.Popen,
                 sh=subprocess.run, verify=None, windows=None):
        self.now, self.sleep, self.healthy, self.spawn, self.sh = now, sleep, healthy, spawn, sh
        self.windows = windows if windows is not None else blocked_windows(CRONTAB.read_text())
        self.verify = verify or self._verify
        self.child = None

    @staticmethod
    def _verify(lock_id: str) -> Path:
        sys.path.insert(0, str(KO_DIR))
        from ko import models
        entry = next((e for e in models.load_lock() if e["id"] == lock_id), None)
        if entry is None:
            raise SystemExit(f"{lock_id!r} is not in models.lock")
        models.verify(entry["file"], models_dir=MODELS_DIR, expect=lock_id)
        return MODELS_DIR / entry["file"]

    # -- the allowed-now rule ---------------------------------------------------
    def refuse_if_blocked(self, max_minutes: int = 0) -> None:
        now = self.now()
        w = blocking(now, self.windows)
        if w:
            raise SystemExit(f"refusing: {w} needs the GPU now (or within {LEAD_MIN} min); see scripts/yoda.crontab")
        nb = next_block(now, self.windows)
        if max_minutes and nb and (nb[0] - now) < dt.timedelta(minutes=max_minutes):
            raise SystemExit(f"refusing: {nb[1]} starts {nb[0]:%a %H:%M}, inside the requested {max_minutes} min")

    # -- the swap ---------------------------------------------------------------
    def _stop_production(self) -> None:
        for name in (PRODUCTION, "qwen9b"):
            self.sh([str(LAUNCH), "stop", name], check=False)
        for _ in range(60):          # the card must be empty before a 14 GB model is loaded
            if not PROD_PID.exists() and not self.healthy(0.5):
                return
            self.sleep(1)

    def start(self, lock_id: str, kv: str | None = None, wait: float = 300) -> None:
        model = self.verify(lock_id)                 # before anything is stopped
        self.refuse_if_blocked()
        kv = kv or kv_choice()
        log(f"swap {PRODUCTION} -> {lock_id} (kv {kv})")
        self._stop_production()
        LOG_DIR.mkdir(exist_ok=True)
        logf = open(LOG_DIR / f"ko_{lock_id}.log", "a")
        proc = self.spawn(server_args(model, kv), stdout=logf, stderr=subprocess.STDOUT, start_new_session=True)
        PID_FILE.parent.mkdir(exist_ok=True)
        PID_FILE.write_text(str(proc.pid))
        for _ in range(int(wait)):
            if proc.poll() is not None:
                raise RuntimeError(f"llama-server exited {proc.returncode} while loading {lock_id} (see ko_{lock_id}.log)")
            if self.healthy():
                log(f"{lock_id} healthy")
                return
            self.sleep(1)
        raise RuntimeError(f"{lock_id} not healthy after {wait}s")

    def stop_ko(self) -> None:
        try:
            pid = int(PID_FILE.read_text())
            os.kill(pid, signal.SIGTERM)
            for _ in range(30):
                os.kill(pid, 0)
                self.sleep(1)
            os.kill(pid, signal.SIGKILL)
        except (OSError, ValueError):
            pass
        PID_FILE.unlink(missing_ok=True)

    def restore(self) -> None:
        self.stop_ko()
        log(f"restore {PRODUCTION}")
        self.sh([str(LAUNCH), "start", PRODUCTION], check=False)
        for _ in range(120):
            if self.healthy():
                log(f"{PRODUCTION} healthy")
                return
            self.sleep(5)
        log(f"{PRODUCTION} NOT healthy after 10 min: Funes is down, fix by hand")
        raise SystemExit(70)

    def run(self, lock_id: str, cmd: list[str], max_minutes: int = 0) -> int:
        """Swap, run `cmd`, restore. Exit 75 when a window forced the run to stop early."""
        self.refuse_if_blocked(max_minutes)
        code, started = 1, False
        try:
            started = True
            self.start(lock_id)
            self.child = self.spawn(cmd, start_new_session=True)
            while self.child.poll() is None:
                nb = next_block(self.now(), self.windows)
                if nb and (nb[0] - self.now()) <= dt.timedelta(minutes=2):
                    log(f"{nb[1]} opens {nb[0]:%H:%M}: stopping the run (resumable)")
                    self.child.terminate()
                    for _ in range(60):
                        if self.child.poll() is not None:
                            break
                        self.sleep(1)
                    else:
                        self.child.kill()
                    code = 75
                    break
                self.sleep(15)
            else:
                code = self.child.returncode
            return code
        finally:
            if started:
                self.restore()


def main(argv=None, swap: Swap | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    sub.add_parser("status")
    sub.add_parser("restore")
    for n in ("start", "loadtest"):
        sub.add_parser(n).add_argument("id")
    r = sub.add_parser("run")
    r.add_argument("id")
    r.add_argument("--max-minutes", type=int, default=0)
    r.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    s = swap or Swap()
    lock = open("/tmp/ko_swap.lock", "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        raise SystemExit("another ko_swap is running")
    if a.cmd == "check":
        s.refuse_if_blocked()
        print("ok: no window now")
        return 0
    if a.cmd == "status":
        nb = next_block(s.now(), s.windows)
        print(f"health={s.healthy()} next blocked: {nb[0]:%a %H:%M} ({nb[1]})" if nb else f"health={s.healthy()}")
        return 0
    if a.cmd == "restore":
        s.restore()
        return 0
    if a.cmd == "start":
        s.start(a.id)
        return 0
    if a.cmd == "loadtest":
        return loadtest(s, a.id)
    while a.command[:1] == ["--max-minutes"] and len(a.command) > 1:  # REMAINDER swallows options after the id
        a.max_minutes, a.command = int(a.command[1]), a.command[2:]
    cmd = a.command[1:] if a.command[:1] == ["--"] else a.command
    if not cmd:
        raise SystemExit("run needs a command after --")
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: sys.exit(143))
    return s.run(a.id, cmd, a.max_minutes)


def long_prompt(words: int = 1350) -> str:
    return " ".join(f"item{i}" for i in range(words)) + "\nHow many items were listed? Answer with a number."


def loadtest(s: Swap, lock_id: str) -> int:
    """Does `lock_id` load with the benchmark flags, and answer a prompt that fills most of ctx 8192 (items tokenise to ~4.8 tokens each, so 1350 words ~ 6.5k tokens)?
    f16 KV first; if that fails, q4_0 — and q4_0 is then recorded for ALL sizes."""
    for kv in ("f16", "q4_0"):
        try:
            s.start(lock_id, kv=kv)
            body = json.dumps({"messages": [{"role": "user", "content": long_prompt()}], "max_tokens": 8,
                               "temperature": 0}).encode()
            req = urllib.request.Request(f"http://127.0.0.1:{PORT}/v1/chat/completions", data=body,
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=600) as r:
                used = json.load(r).get("usage", {})
            KV_FILE.write_text(kv)
            log(f"loadtest {lock_id}: OK with kv {kv}, prompt_tokens {used.get('prompt_tokens')}; "
                f"recorded {KV_FILE} = {kv} for every size")
            return 0
        except Exception as e:
            log(f"loadtest {lock_id} kv {kv} FAILED: {type(e).__name__}: {e}")
        finally:
            s.restore()
    log(f"loadtest {lock_id}: does not fit on this card with either KV type")
    return 2


if __name__ == "__main__":
    sys.exit(main())
