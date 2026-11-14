#!/usr/bin/env python3
"""funes-guardian — undo that survives Funes (self-healing plan, phase H1).

A change to the installation is a transaction: before it, the files it may
touch are snapshotted; after it, the change is *applied* and watched for a
confirmation window. If Funes stops answering, or an incident opens, the
guardian puts the snapshot back by itself — the way a router's
`commit confirmed` reverts unless the change proves good. A person can undo
any change by number: "deshacer N" on WhatsApp, in the web chat, or
`funes_guardian.py undo N`.

It is a separate program on purpose. If a change breaks the binary, an agent
YAML or the config, the thing that would roll it back cannot be the thing that
is broken. So this runs from its own systemd timer (funes-guardian.timer),
reads WhatsApp's store itself, and sends through the bridge itself; nothing
here needs Funes to be up. Its state lives in data/guardian/, which nothing
Funes starts can write (scripts and MCP servers get only their own grants).

Who makes transactions today:
  create_agent  — Funes editing its own agents/*.yaml (agent-doctor), the one
                  live self-modification the runtime has (src/core/guardian.h);
  deploy        — `funes_guardian.py deploy`: git pull --ff-only, build,
                  restart, watched; undone = git reset to the old commit and
                  the old binary back.
Runbooks (H2) will be the next.

Usage:
  funes_guardian.py begin --reason R --actor A --path P [--path …] [--restart UNIT …]
  funes_guardian.py applied N [--confirm-after SECONDS]
  funes_guardian.py undo N [--by WHO]
  funes_guardian.py list
  funes_guardian.py watch                  # the timer: verify, auto-undo, WhatsApp
  funes_guardian.py deploy [--confirm-after SECONDS]
"""

import argparse
import fcntl
import io
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
from contextlib import contextmanager
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_CONFIRM = 900          # seconds a change is watched before it is final
DEPLOY_CONFIRM = 600
FAILURES_TO_UNDO = 2           # consecutive failed checks (the timer runs every minute)
ABANDON_AFTER = 3600           # begun, never applied: the change did not happen
KEEP_DAYS = 30                 # snapshots of finished changes
# An incident whose key also opened in this many days before a change is the
# system's weather (a TV that does not answer), not something the change did.
# 2026-11-14: the 7.20.0 deploy was undone over the TV job's usual timeout.
HISTORY_DAYS = 7

UNDO_RE = re.compile(r"^\s*(?:deshacer|undo)\s+#?(\d{1,6})\s*[.!]?\s*$", re.IGNORECASE)


class GuardianError(Exception):
    pass


def parse_undo(text: str):
    """'deshacer 3' / 'undo #3' alone in a message → 3; anything else → None."""
    m = UNDO_RE.match(text or "")
    return int(m.group(1)) if m else None


def log(msg: str):
    print(f"[{time.strftime('%Y-%m-%dT%H:%M:%S')}] {msg}", file=sys.stderr, flush=True)


def run_cmd(argv, cwd=None, timeout=600):
    try:
        p = subprocess.run(argv, cwd=cwd, timeout=timeout, capture_output=True, text=True)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except (OSError, subprocess.TimeoutExpired) as e:
        return 1, str(e)


class Guardian:
    def __init__(self, root, run=run_cmd, now=time.time, funes_up=lambda: True,
                 send=lambda jid, text: False, admin_jids=lambda: [], db_path=None):
        self.root = Path(root).resolve()
        self.state = self.root / "data" / "guardian"
        self.txns = self.state / "txns"
        self.snaps = self.state / "snap"
        self.run = run
        self.now = now
        self.funes_up = funes_up
        self.send = send
        self.admin_jids = admin_jids
        self.db_path = Path(db_path) if db_path else self.root / "data" / "memory.db"
        for d in (self.txns, self.snaps):
            d.mkdir(parents=True, exist_ok=True)

    # ── state ────────────────────────────────────────────────────────────────

    @contextmanager
    def locked(self):
        with open(self.state / "guardian.lock", "w") as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(f, fcntl.LOCK_UN)

    def load(self, n: int):
        p = self.txns / f"{n}.json"
        return json.loads(p.read_text(encoding="utf-8")) if p.exists() else None

    def save(self, t: dict):
        p = self.txns / f"{t['id']}.json"
        tmp = p.with_suffix(".tmp")
        tmp.write_text(json.dumps(t, indent=1, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, p)

    def all(self):
        out = []
        for p in self.txns.glob("*.json"):
            try:
                out.append(json.loads(p.read_text(encoding="utf-8")))
            except (OSError, ValueError):
                log(f"unreadable transaction {p.name}, skipped")
        return sorted(out, key=lambda t: t["id"])

    def rel(self, path: str) -> str:
        """A path inside the checkout, outside the guardian's own state."""
        p = Path(path)
        full = (p if p.is_absolute() else self.root / p).resolve()
        if full == self.root or self.root not in full.parents:
            raise GuardianError(f"{path}: not inside {self.root}")
        if full == self.state or self.state in full.parents:
            raise GuardianError(f"{path}: the guardian's own state is not a change target")
        return str(full.relative_to(self.root))

    def notify(self, text: str):
        for jid in self.admin_jids():
            if not self.send(jid, text):
                log(f"could not send a notice to {jid}")

    # ── transactions ─────────────────────────────────────────────────────────

    def begin(self, reason, actor, paths, restart=(), git_head=None) -> int:
        if not paths:
            raise GuardianError("a change needs at least one path to protect")
        rels = [self.rel(p) for p in paths]
        with self.locked():
            n = max([t["id"] for t in self.all()] + [0]) + 1
            absent = []
            with tarfile.open(self.snaps / f"{n}.tar", "w") as tar:
                for r in rels:
                    if (self.root / r).exists():
                        tar.add(self.root / r, arcname=r)
                    else:
                        absent.append(r)
            self.save({"id": n, "reason": reason, "actor": actor, "paths": rels,
                       "absent": absent, "restart": list(restart), "git_head": git_head,
                       "state": "open", "created_at": int(self.now())})
        log(f"#{n} begun: {reason} ({actor}) — {', '.join(rels)}")
        return n

    def applied(self, n: int, confirm_after=DEFAULT_CONFIRM):
        with self.locked():
            t = self.load(n)
            if not t:
                raise GuardianError(f"no change #{n}")
            if t["state"] != "open":
                raise GuardianError(f"change #{n} is {t['state']}, not open")
            t.update(state="applied", applied_at=int(self.now()),
                     confirm_after=int(confirm_after), failures=0)
            self.save(t)
        log(f"#{n} applied; watched for {confirm_after}s")
        if t["actor"].startswith("funes:") or t["actor"] == "deploy":
            what = ", ".join(t["paths"])
            self.notify(f"🔧 Cambio #{n}: {t['reason']} ({what}). Lo vigilo "
                        f"{int(confirm_after) // 60} min y lo vuelvo atrás solo si algo se "
                        f"rompe. Para deshacerlo ya: «deshacer {n}».")

    def restore(self, t: dict):
        snap = self.snaps / f"{t['id']}.tar"
        with tarfile.open(snap) as tar:
            members = tar.getmembers()
            for r in t["paths"]:
                target = self.root / r
                if target.is_dir() and not target.is_symlink():
                    shutil.rmtree(target)
                elif target.exists() or target.is_symlink():
                    target.unlink()
                if r in t["absent"]:
                    continue
                mine = [m for m in members if m.name == r or m.name.startswith(r + "/")]
                for m in mine:
                    if m.issym() or m.islnk() or m.name.startswith("/") or ".." in Path(m.name).parts:
                        raise GuardianError(f"snapshot #{t['id']}: refusing member {m.name}")
                kwargs = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
                tar.extractall(self.root, members=mine, **kwargs)

    def restart_units(self, units):
        for unit in units:
            if unit == "funes":
                # Possibly the process asking (a "deshacer" in the web chat):
                # restart it a few seconds from now, after it has answered.
                argv = ["systemd-run", "--user", "--on-active=5", "--collect",
                        "systemctl", "--user", "restart", unit]
            else:
                argv = ["systemctl", "--user", "restart", unit]
            rc, out = self.run(argv, cwd=str(self.root), timeout=60)
            if rc != 0:
                log(f"restart {unit} failed: {out.strip()[:200]}")

    def undo(self, n: int, by: str = "", reason: str = ""):
        with self.locked():
            t = self.load(n)
            if not t:
                return False, f"No hay un cambio #{n}."
            if t["state"] == "undone":
                return False, f"El cambio #{n} ({t['reason']}) ya estaba deshecho."
            try:
                self.restore(t)
                if t.get("git_head"):
                    rc, out = self.run(["git", "reset", "--hard", t["git_head"]],
                                       cwd=str(self.root), timeout=120)
                    if rc != 0:
                        raise GuardianError(f"git reset failed: {out.strip()[:200]}")
            except (OSError, tarfile.TarError, GuardianError) as e:
                log(f"#{n} undo FAILED: {e}")
                return False, f"No pude deshacer el cambio #{n}: {e}"
            t.update(state="undone", undone_at=int(self.now()), undone_by=by,
                     undo_reason=reason or f"asked by {by or 'someone'}")
            self.save(t)
        self.restart_units(t.get("restart", []))
        log(f"#{n} undone ({t['undo_reason']})")
        return True, (f"↩️ Cambio #{n} deshecho ({t['reason']}): volví "
                      f"{', '.join(t['paths'])} a como estaban"
                      + (f" y el código a {t['git_head'][:7]}" if t.get("git_head") else "")
                      + (f"; reinicio {', '.join(t['restart'])}" if t.get("restart") else "") + ".")

    # ── the timer ────────────────────────────────────────────────────────────

    def new_incidents(self, since: int):
        if not self.db_path.exists():
            return []
        try:
            con = sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True, timeout=5)
            try:
                return con.execute(
                    "SELECT key, summary FROM incidents i WHERE opened_at >= ? AND resolved_at = 0 "
                    "AND NOT EXISTS (SELECT 1 FROM incidents h WHERE h.key = i.key "
                    "AND h.opened_at < ? AND h.opened_at >= ?) ORDER BY id",
                    (since, since, since - HISTORY_DAYS * 86400)).fetchall()
            finally:
                con.close()
        except sqlite3.Error:
            return []   # no incidents table (probes off, or an older Funes)

    def health(self, t: dict) -> str:
        # Incidents new to this change only (see HISTORY_DAYS).
        """"" when healthy, else why not."""
        if not self.funes_up():
            return "Funes no responde (Funes does not answer)"
        found = self.new_incidents(t["applied_at"])
        if found:
            key, summary = found[0]
            return f"apareció un problema nuevo: {summary} ({key})"
        return ""

    def watch(self):
        now = int(self.now())
        for t in self.all():
            if t["state"] == "open" and now - t["created_at"] > ABANDON_AFTER:
                with self.locked():
                    t["state"] = "abandoned"
                    self.save(t)
                log(f"#{t['id']} abandoned: begun, never applied")
            elif t["state"] == "applied":
                why = self.health(t)
                if why:
                    t["failures"] = t.get("failures", 0) + 1
                    t["last_failure"] = why
                    with self.locked():
                        self.save(t)
                    log(f"#{t['id']} check failed ({t['failures']}/{FAILURES_TO_UNDO}): {why}")
                    if t["failures"] >= FAILURES_TO_UNDO:
                        ok, msg = self.undo(t["id"], by="guardian", reason=why)
                        self.notify(f"Deshice solo el cambio #{t['id']} ({t['reason']}) porque {why}.\n{msg}"
                                    if ok else msg)
                else:
                    if t.get("failures"):
                        t["failures"] = 0
                        with self.locked():
                            self.save(t)
                    if now >= t["applied_at"] + t["confirm_after"]:
                        with self.locked():
                            t["state"] = "confirmed"
                            t["confirmed_at"] = now
                            self.save(t)
                        log(f"#{t['id']} confirmed")
        self.prune(now)

    def prune(self, now: int):
        """Snapshots of changes finished more than KEEP_DAYS ago; the record stays."""
        for t in self.all():
            done = t.get("confirmed_at") or t.get("undone_at")
            if t["state"] in ("confirmed", "undone", "abandoned") and done and now - done > KEEP_DAYS * 86400:
                snap = self.snaps / f"{t['id']}.tar"
                if snap.exists():
                    snap.unlink()

    def poll_whatsapp(self, store: Path):
        """"deshacer N" sent to Funes's number by an admin. Read here, not by the
        autoresponder (which skips it): it has to work while Funes is down."""
        state_file = self.state / "whatsapp.json"
        seen = json.loads(state_file.read_text()) if state_file.exists() else {"last": "", "ids": []}
        if not Path(store).exists():
            return
        try:
            con = sqlite3.connect(f"file:{store}?mode=ro", uri=True, timeout=5)
            try:
                rows = con.execute("SELECT id, chat_jid, content, timestamp FROM messages "
                                   "WHERE is_from_me = 0 AND timestamp >= ? ORDER BY timestamp",
                                   (seen["last"],)).fetchall()
            finally:
                con.close()
        except sqlite3.Error as e:
            log(f"cannot read the WhatsApp store: {e}")
            return
        if not seen["last"]:
            # First run: start from now, do not replay history.
            seen["last"] = rows[-1][3] if rows else ""
            seen["ids"] = [r[0] for r in rows if r[3] == seen["last"]]
            state_file.write_text(json.dumps(seen))
            rows = []
        admins = set(self.admin_jids())
        for msg_id, jid, content, ts in rows:
            if msg_id in seen["ids"] and ts == seen["last"]:
                continue
            if ts != seen["last"]:
                seen["last"], seen["ids"] = ts, []
            seen["ids"].append(msg_id)
            n = parse_undo(content or "")
            if n is None or jid not in admins:
                continue
            ok, msg = self.undo(n, by=jid)
            self.send(jid, msg)
        state_file.write_text(json.dumps(seen))

    # ── deploy ───────────────────────────────────────────────────────────────

    def deploy(self, confirm_after=DEPLOY_CONFIRM):
        rc, dirty = self.run(["git", "status", "--porcelain", "--untracked-files=no"],
                             cwd=str(self.root), timeout=60)
        if rc != 0 or dirty.strip():
            raise GuardianError("the checkout has local changes; a deploy is git pull --ff-only, "
                                "and an undo is git reset --hard, which would lose them:\n" + dirty)
        old = self.run(["git", "rev-parse", "HEAD"], cwd=str(self.root), timeout=30)[1].strip()
        n = self.begin("deploy", "deploy", ["bin/funes"], restart=["funes"], git_head=old)
        rc, out = self.run(["git", "pull", "--ff-only", "origin", "main"], cwd=str(self.root), timeout=180)
        new = self.run(["git", "rev-parse", "HEAD"], cwd=str(self.root), timeout=30)[1].strip()
        if rc != 0 or new == old:
            with self.locked():
                (self.txns / f"{n}.json").unlink()
                (self.snaps / f"{n}.tar").unlink(missing_ok=True)
            if rc != 0:
                raise GuardianError(f"git pull failed: {out.strip()[:300]}")
            log("already up to date; nothing deployed")
            return None
        rc, out = self.run(["cmake", "--build", "build", f"-j{os.cpu_count() or 2}"],
                           cwd=str(self.root), timeout=3600)
        if rc != 0:
            self.undo(n, by="deploy", reason="the build failed")
            raise GuardianError(f"build failed, back at {old[:7]}:\n{out.strip()[-800:]}")
        rc, out = self.run(["systemctl", "--user", "restart", "funes"], cwd=str(self.root), timeout=120)
        with self.locked():
            t = self.load(n)
            t["reason"] = f"deploy {old[:7]}→{new[:7]}"
            self.save(t)
        self.applied(n, confirm_after)
        return n

    def list(self, limit=20):
        return list(reversed(self.all()))[:limit]


# ── the real world ───────────────────────────────────────────────────────────

def load_config(root: Path) -> dict:
    cfg = {}
    for name in ("funes.conf", "funes.local"):
        try:
            for line in (root / "config" / name).read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k.strip()] = v.strip().strip('"').strip("'")
        except OSError:
            pass
    cfg.update({k: v for k, v in os.environ.items() if k.startswith(("FUNES_", "WHATSAPP_"))})
    return cfg


def real_guardian(root: Path) -> tuple:
    cfg = load_config(root)
    db = Path(cfg.get("FUNES_DB") or root / "data" / "memory.db")
    port = cfg.get("FUNES_PORT", "8484")
    bridge = cfg.get("WHATSAPP_BRIDGE_URL", "http://localhost:8091").rstrip("/")

    def funes_up():
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/auth/status", timeout=8) as r:
                return r.status == 200
        except (urllib.error.URLError, OSError, ValueError):
            return False

    def send(jid, text):
        req = urllib.request.Request(f"{bridge}/api/send", method="POST",
                                     data=json.dumps({"recipient": jid, "message": text}).encode(),
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read().decode() or "{}").get("success", True) is not False
        except (urllib.error.URLError, OSError, ValueError):
            return False

    def admin_jids():
        try:
            con = sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=5)
            try:
                return [r[0] for r in con.execute(
                    "SELECT j.chat_jid FROM jid_users j JOIN users u ON u.id = j.user_id "
                    "WHERE u.role = 'admin' ORDER BY u.id")]
            finally:
                con.close()
        except sqlite3.Error:
            return []

    store = Path(cfg.get("WHATSAPP_DB_PATH") or root / "data" / "whatsapp" / "store-funes" / "messages.db")
    g = Guardian(root, funes_up=funes_up, send=send, admin_jids=admin_jids, db_path=db)
    return g, store


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Undo that survives Funes (self-healing H1).")
    ap.add_argument("--root", default=str(REPO_ROOT))
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("begin")
    b.add_argument("--reason", required=True)
    b.add_argument("--actor", default="cli")
    b.add_argument("--path", action="append", required=True)
    b.add_argument("--restart", action="append", default=[])
    a = sub.add_parser("applied")
    a.add_argument("id", type=int)
    a.add_argument("--confirm-after", type=int, default=DEFAULT_CONFIRM)
    u = sub.add_parser("undo")
    u.add_argument("id", type=int)
    u.add_argument("--by", default="cli")
    sub.add_parser("list")
    sub.add_parser("watch")
    d = sub.add_parser("deploy")
    d.add_argument("--confirm-after", type=int, default=DEPLOY_CONFIRM)
    args = ap.parse_args(argv)

    g, store = real_guardian(Path(args.root))
    try:
        if args.cmd == "begin":
            print(json.dumps({"id": g.begin(args.reason, args.actor, args.path, args.restart)}))
        elif args.cmd == "applied":
            g.applied(args.id, args.confirm_after)
            print(json.dumps({"ok": True}))
        elif args.cmd == "undo":
            ok, msg = g.undo(args.id, by=args.by)
            print(json.dumps({"ok": ok, "message": msg}, ensure_ascii=False))
            return 0 if ok else 1
        elif args.cmd == "list":
            for t in g.list():
                when = time.strftime("%m-%d %H:%M", time.localtime(t["created_at"]))
                print(f"#{t['id']:<4} {when}  {t['state']:<9} {t['actor']:<20} {t['reason']}")
        elif args.cmd == "watch":
            g.watch()
            g.poll_whatsapp(store)
        elif args.cmd == "deploy":
            n = g.deploy(args.confirm_after)
            print("nothing to deploy" if n is None else
                  f"deployed as change #{n}; watched for {args.confirm_after // 60} min "
                  f"(funes_guardian.py undo {n} to go back now)")
    except GuardianError as e:
        print(json.dumps({"ok": False, "error": str(e)}, ensure_ascii=False))
        log(str(e))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
