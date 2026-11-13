#!/usr/bin/env python3
"""
linkedin_token.py — keep post_tweet.py's LinkedIn token from expiring unseen.

A member token lasts 60 days and LinkedIn issues refresh tokens only to its
Marketing Developer Platform partners, so this app has none: every 60 days a
person has to approve the app again in a browser. Until 2026-11-12 the only
reminder was a "Warning: expires in N day(s)" line in post_log.txt, which
nobody reads; the token lapsed at 19:15 and the posts failed until someone
noticed LinkedIn was quiet.

So the reminder goes where it will be read, and approving is one tap:

    check   (cron, daily) — seven days before expiry, or once expired, send
            a WhatsApp message to the admin with a renewal link.
    serve   (systemd unit linkedin-callback) — answer LinkedIn's redirect
            after that approval: exchange the code, write the new token into
            data/newsletter/.env. Listens on 127.0.0.1 only; tailscale funnel
            publishes /linkedin on yoda's https name, because LinkedIn's
            redirect lands in the phone's browser, not on yoda.
    url     print a renewal link now (same flow, no message).

The redirect URI (LINKEDIN_REDIRECT_URI in config/funes.local, e.g.
https://yoda.<tailnet>.ts.net/linkedin/callback) must also be listed in the
LinkedIn app's Auth settings, or LinkedIn refuses to send the browser there.

The callback is on the public internet, so it does nothing for a request
that does not carry the one pending `state`: a random 256-bit value written
by `check`/`url`, valid for STATE_TTL, spent on first use. Nothing a request
carries is echoed into the page.

Credentials stay where post_tweet.py reads them: <dir>/.env (LI_CLIENT_ID,
LI_CLIENT_SECRET, LI_ACCESS_TOKEN, LI_TOKEN_EXPIRY).
"""

import argparse
import hmac
import http.server
import json
import os
import secrets
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DIR = REPO_ROOT / "data" / "newsletter"

SCOPES = "openid profile w_member_social"
AUTH_URL = "https://www.linkedin.com/oauth/v2/authorization"
TOKEN_URL = "https://www.linkedin.com/oauth/v2/accessToken"
WARN_DAYS = 7
STATE_TTL = 3 * 86400
DEFAULT_PORT = 8492


def log(msg):
    print(f"[{datetime.now().isoformat(timespec='seconds')}] {msg}", flush=True)


# ── Config ───────────────────────────────────────────────────────────────────

def load_kv(path: Path) -> dict:
    """KEY=VALUE lines; blank lines and # comments skipped."""
    out = {}
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        out[key.strip()] = value.strip()
    return out


def load_config() -> dict:
    """funes.conf then funes.local, later wins; the process environment wins
    over both. Same order as whatsapp_autoresponder.py."""
    cfg_dir = Path(os.environ.get("FUNES_CONFIG_DIR") or REPO_ROOT / "config").expanduser()
    cfg = {}
    for name in ("funes.conf", "funes.local"):
        cfg.update(load_kv(cfg_dir / name))
    cfg.update({k: v for k, v in os.environ.items()
                if k.startswith(("LINKEDIN_", "WHATSAPP_", "FUNES_"))})
    return cfg


def save_env_values(path: Path, updates: dict) -> None:
    """Replace (or append) KEY=value lines in place. Written in place rather
    than renamed over: the unit bind-mounts this one file, and a rename onto
    a mount point fails."""
    lines = path.read_text().splitlines() if path.exists() else []
    seen = set()
    for i, line in enumerate(lines):
        key = line.partition("=")[0].strip()
        if key in updates and not line.lstrip().startswith("#"):
            lines[i] = f"{key}={updates[key]}"
            seen.add(key)
    lines += [f"{k}={v}" for k, v in updates.items() if k not in seen]
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


# ── State: the one pending renewal ───────────────────────────────────────────

def state_path(work_dir: Path) -> Path:
    return work_dir / "linkedin" / "renew_state.json"


def pending_state(work_dir: Path, now: float):
    try:
        data = json.loads(state_path(work_dir).read_text())
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or not data.get("state"):
        return None
    if now - float(data.get("created", 0)) > STATE_TTL:
        return None
    return data


def new_or_pending_state(work_dir: Path, now: float) -> str:
    """The pending state if one is still valid — so yesterday's link keeps
    working — else a fresh one."""
    data = pending_state(work_dir, now)
    if data:
        return data["state"]
    state = secrets.token_urlsafe(32)
    path = state_path(work_dir)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"state": state, "created": now}))
    os.chmod(path, 0o600)
    return state


def spend_state(work_dir: Path, offered: str, now: float) -> bool:
    """True once for the pending state; it is cleared before anything else
    happens, so a replayed callback finds nothing."""
    data = pending_state(work_dir, now)
    if not data or not offered:
        return False
    if not hmac.compare_digest(data["state"].encode(), offered.encode()):
        return False
    state_path(work_dir).write_text("{}")
    return True


def auth_url(client_id: str, redirect_uri: str, state: str) -> str:
    return AUTH_URL + "?" + urllib.parse.urlencode({
        "response_type": "code",
        "client_id": client_id,
        "redirect_uri": redirect_uri,
        "scope": SCOPES,
        "state": state,
    })


# ── check ────────────────────────────────────────────────────────────────────

def days_left(expiry: int, now: float) -> float:
    return (expiry - now) / 86400


def alert_text(left: float, expiry: int, link: str) -> str:
    if left <= 0:
        head = "⚠️ El token de LinkedIn venció: los posts del newsletter están fallando."
    else:
        head = (f"🔑 El token de LinkedIn vence en {max(1, int(left))} día(s) "
                f"({datetime.fromtimestamp(expiry):%d/%m %H:%M}).")
    return (f"{head}\n\nPara renovarlo, abrí este link y aprobá la app "
            f"(sirve 3 días):\n{link}")


def admin_jid(cfg: dict) -> str:
    """LINKEDIN_ALERT_JID, else the jid mapped to the first admin account."""
    if cfg.get("LINKEDIN_ALERT_JID"):
        return cfg["LINKEDIN_ALERT_JID"]
    db = cfg.get("FUNES_DB") or str(REPO_ROOT / "data" / "memory.db")
    try:
        con = sqlite3.connect(f"file:{Path(db).expanduser()}?mode=ro", uri=True)
        row = con.execute(
            "SELECT j.chat_jid FROM jid_users j JOIN users u ON u.id = j.user_id "
            "WHERE u.role = 'admin' ORDER BY u.id LIMIT 1").fetchone()
        con.close()
        return row[0] if row else ""
    except sqlite3.Error as e:
        log(f"cannot read the admin's jid from {db}: {e}")
        return ""


def send_whatsapp(bridge: str, jid: str, text: str) -> bool:
    req = urllib.request.Request(
        f"{bridge}/api/send",
        data=json.dumps({"recipient": jid, "message": text}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            result = json.loads(resp.read().decode() or "{}")
            if not result.get("success", True):
                log(f"bridge refused the message: {result}")
                return False
            return True
    except (urllib.error.URLError, TimeoutError, ValueError) as e:
        log(f"cannot reach the WhatsApp bridge at {bridge}: {e}")
        return False


def cmd_check(work_dir: Path, cfg: dict, now: float, send=send_whatsapp) -> int:
    creds = load_kv(work_dir / ".env")
    expiry = int(creds.get("LI_TOKEN_EXPIRY") or 0)
    if not expiry:
        log("no LI_TOKEN_EXPIRY in .env; nothing to check")
        return 1
    left = days_left(expiry, now)
    if left > WARN_DAYS:
        log(f"LinkedIn token OK: {left:.1f} days left")
        return 0
    redirect = cfg.get("LINKEDIN_REDIRECT_URI", "")
    if not redirect or not creds.get("LI_CLIENT_ID"):
        log("LINKEDIN_REDIRECT_URI (config/funes.local) or LI_CLIENT_ID (.env) "
            "missing; cannot build a renewal link")
        return 1
    jid = admin_jid(cfg)
    if not jid:
        log("no admin jid to alert (set LINKEDIN_ALERT_JID)")
        return 1
    link = auth_url(creds["LI_CLIENT_ID"], redirect, new_or_pending_state(work_dir, now))
    bridge = cfg.get("WHATSAPP_BRIDGE_URL", "http://localhost:8091")
    if not send(bridge, jid, alert_text(left, expiry, link)):
        return 1
    log(f"LinkedIn token: {left:.1f} days left; renewal link sent on WhatsApp")
    return 0


def cmd_url(work_dir: Path, cfg: dict, now: float) -> int:
    creds = load_kv(work_dir / ".env")
    redirect = cfg.get("LINKEDIN_REDIRECT_URI", "")
    if not redirect or not creds.get("LI_CLIENT_ID"):
        print("LINKEDIN_REDIRECT_URI or LI_CLIENT_ID missing", file=sys.stderr)
        return 1
    print(auth_url(creds["LI_CLIENT_ID"], redirect, new_or_pending_state(work_dir, now)))
    return 0


# ── serve ────────────────────────────────────────────────────────────────────

def exchange_code(code: str, creds: dict, redirect: str) -> dict:
    body = urllib.parse.urlencode({
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": redirect,
        "client_id": creds.get("LI_CLIENT_ID", ""),
        "client_secret": creds.get("LI_CLIENT_SECRET", ""),
    }).encode()
    req = urllib.request.Request(
        TOKEN_URL, data=body, method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode())


PAGE = ("<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width'>"
        "<title>LinkedIn</title><body style='font:18px system-ui;margin:2em'>"
        "<p>{}</p>")


def handle_callback(query: dict, work_dir: Path, cfg: dict, now: float,
                    exchange=exchange_code):
    """(status, message) for one callback. Messages are fixed strings plus a
    date: nothing from the request reaches the page."""
    if not spend_state(work_dir, (query.get("state") or [""])[0], now):
        return 404, "Este link ya no es válido. Pedí uno nuevo."
    if query.get("error"):
        log(f"LinkedIn refused the approval: {query['error'][0][:80]!r}")
        return 400, "LinkedIn no autorizó la app. El token sigue igual."
    code = (query.get("code") or [""])[0]
    if not code:
        return 400, "Falta el código de LinkedIn."
    env_path = work_dir / ".env"
    creds = load_kv(env_path)
    try:
        data = exchange(code, creds, cfg.get("LINKEDIN_REDIRECT_URI", ""))
    except (urllib.error.URLError, TimeoutError, ValueError) as e:
        log(f"token exchange failed: {e}")
        return 502, "LinkedIn no entregó el token. Probá de nuevo con un link nuevo."
    token = data.get("access_token", "")
    if not token:
        log("token exchange answered without an access_token")
        return 502, "LinkedIn no entregó el token. Probá de nuevo con un link nuevo."
    expiry = int(now) + int(data.get("expires_in", 60 * 86400))
    save_env_values(env_path, {
        "LI_ACCESS_TOKEN": token,
        "LI_REFRESH_TOKEN": data.get("refresh_token", ""),
        "LI_TOKEN_EXPIRY": str(expiry),
    })
    log(f"LinkedIn token renewed; expires {datetime.fromtimestamp(expiry):%Y-%m-%d %H:%M}")
    return 200, f"Listo: token de LinkedIn renovado, vence el {datetime.fromtimestamp(expiry):%d/%m/%Y}."


def make_handler(work_dir: Path, cfg: dict):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            # tailscale may or may not strip the /linkedin mount point.
            if not parsed.path.rstrip("/").endswith("/callback"):
                status, msg = 404, "No encontrado."
            else:
                status, msg = handle_callback(
                    urllib.parse.parse_qs(parsed.query), work_dir, cfg, time.time())
            log(f"GET {parsed.path} -> {status}")
            body = PAGE.format(msg).encode()
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass                         # the query carries the code; log() above omits it

    return Handler


def cmd_serve(work_dir: Path, cfg: dict) -> int:
    port = int(cfg.get("LINKEDIN_CALLBACK_PORT") or DEFAULT_PORT)
    server = http.server.HTTPServer(("127.0.0.1", port), make_handler(work_dir, cfg))
    log(f"listening on 127.0.0.1:{port} for {cfg.get('LINKEDIN_REDIRECT_URI') or '(no LINKEDIN_REDIRECT_URI)'}")
    server.serve_forever()
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("command", choices=["check", "serve", "url"])
    ap.add_argument("--dir", type=Path, default=DEFAULT_DIR,
                    help="the newsletter directory holding .env")
    args = ap.parse_args(argv)
    cfg = load_config()
    if args.command == "check":
        return cmd_check(args.dir, cfg, time.time())
    if args.command == "url":
        return cmd_url(args.dir, cfg, time.time())
    return cmd_serve(args.dir, cfg)


if __name__ == "__main__":
    sys.exit(main())
