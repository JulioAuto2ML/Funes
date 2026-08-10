#!/usr/bin/env python3
"""Poll the WhatsApp bridge's local SQLite store for new incoming messages
from whitelisted chats, ask Funes for a reply, and send it back.

Nothing here is on any critical path Funes itself runs — this is a small
standalone loop meant to run as its own systemd --user service (see
scripts/whatsapp-autoresponder.service). It never touches Funes' internals
directly; it only talks to Funes' own HTTP API (/api/chat), the same way any
other client would.

Safety model: the whatsapp-autoresponder agent (agents/whatsapp-autoresponder.yaml)
has no send capability of its own — only `recall`/`remember`/`read_file`. This
script is the only thing that ever calls the bridge's /api/send, and only for
chat_jids already on WHATSAPP_WHITELIST, always replying into the exact chat
the incoming message came from. The LLM never chooses a recipient.

Documents ("document" attachments — PDFs, plain text files, etc.) and photos
("image" attachments) sent by a whitelisted contact are downloaded via the
bridge's /api/download and copied into WHATSAPP_UPLOAD_DIR, where the
whatsapp-autoresponder agent's `read_file` tool (scoped to that directory
only, see workspace_dir in its yaml) can read them — PDFs and plain text via
text extraction, images via the vision-capable backend if one is configured.
Files older than WHATSAPP_UPLOAD_MAX_AGE_DAYS are deleted automatically.
Audio and video attachments are still ignored.
"""
import json
import os
import re
import shutil
import sqlite3
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
STATE_PATH = Path(os.path.expanduser("~/.funes/whatsapp_autoresponder_state.json"))


def load_config():
    """Read config/funes.conf then config/funes.local (KEY=VALUE lines,
    later file wins), same precedence Funes itself documents — but real
    process env vars always win over both."""
    cfg = {}
    for name in ("config/funes.conf", "config/funes.local"):
        path = REPO_ROOT / name
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            cfg[key.strip()] = value.strip()
    cfg.update({k: v for k, v in os.environ.items() if k in cfg or k.startswith("WHATSAPP_") or k.startswith("FUNES_")})
    return cfg


CFG = load_config()

DB_PATH = CFG.get(
    "WHATSAPP_DB_PATH",
    str(REPO_ROOT / "third-party/whatsapp-mcp/whatsapp-bridge/store/messages.db"),
)
FUNES_API_URL = CFG.get("FUNES_API_URL", "http://localhost:8484")
BRIDGE_API_URL = CFG.get("WHATSAPP_BRIDGE_URL", "http://localhost:8090")
AGENT_NAME = CFG.get("WHATSAPP_AUTORESPONDER_AGENT", "whatsapp-autoresponder")
POLL_SECONDS = float(CFG.get("WHATSAPP_POLL_SECONDS", "5"))
CHAT_TIMEOUT = float(CFG.get("WHATSAPP_CHAT_TIMEOUT_SECONDS", "120"))
WHITELIST = {
    jid.strip() for jid in CFG.get("WHATSAPP_WHITELIST", "").split(",") if jid.strip()
}

UPLOAD_DIR = Path(CFG.get("WHATSAPP_UPLOAD_DIR", str(REPO_ROOT / "data" / "whatsapp-uploads")))
MAX_MEDIA_BYTES = int(CFG.get("WHATSAPP_MAX_MEDIA_BYTES", str(20 * 1024 * 1024)))
UPLOAD_MAX_AGE_DAYS = int(CFG.get("WHATSAPP_UPLOAD_MAX_AGE_DAYS", "30"))
CLEANUP_INTERVAL_SECONDS = 24 * 3600


def log(msg):
    print(f"[whatsapp-autoresponder] {msg}", flush=True)


def newest_existing_state() -> dict:
    """The bridge's own max timestamp (in its own string format, so later
    string comparisons against that column compare like with like) plus the
    ids already sitting at that timestamp — used as the first-run watermark
    so we don't reply to the newest pre-existing message too (fetch_new_messages
    only excludes ids already recorded at last_timestamp, so this has to be
    seeded, not left empty, or the boundary message gets answered once)."""
    conn = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True, timeout=5)
    try:
        max_ts = conn.execute("SELECT MAX(timestamp) FROM messages").fetchone()[0]
        if not max_ts:
            return {"last_timestamp": "", "last_timestamp_ids": []}
        ids = [r[0] for r in conn.execute(
            "SELECT id FROM messages WHERE timestamp = ?", (max_ts,)
        ).fetchall()]
        return {"last_timestamp": max_ts, "last_timestamp_ids": ids}
    finally:
        conn.close()


def load_state():
    if STATE_PATH.exists():
        try:
            return json.loads(STATE_PATH.read_text())
        except (json.JSONDecodeError, OSError):
            pass
    # First run: start after the newest message already in the DB, so we
    # don't reply to the entire backlog (or even just its last message).
    return newest_existing_state()


def save_state(state):
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    STATE_PATH.write_text(json.dumps(state))


def sanitize_session(chat_jid: str) -> str:
    session = "whatsapp_" + re.sub(r"[^A-Za-z0-9_-]", "_", chat_jid)
    return session[:64]


def sanitize_path_component(name: str, max_len: int, keep_dots: bool) -> str:
    pattern = r"[^A-Za-z0-9_.-]" if keep_dots else r"[^A-Za-z0-9_-]"
    safe = re.sub(pattern, "_", name)[:max_len]
    return safe or "_"


def download_media(message_id: str, chat_jid: str) -> dict:
    payload = json.dumps({"message_id": message_id, "chat_jid": chat_jid}).encode()
    req = urllib.request.Request(
        f"{BRIDGE_API_URL}/api/download", data=payload,
        headers={"Content-Type": "application/json"}, method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read().decode())
        except (json.JSONDecodeError, OSError):
            return {"success": False, "message": f"HTTP {e.code}"}
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
        return {"success": False, "message": str(e)}


def handle_incoming_media(msg_id: str, chat_jid: str, media_type: str, filename: str,
                           file_length: int, caption: str) -> str:
    """Download a WhatsApp "document" or "image" attachment and copy it into
    UPLOAD_DIR, returning the text to hand to Funes (a `[Document received:
    <path>]` / `[Photo received: <path>]` marker the agent's read_file tool
    can act on, plus the original caption if any). On any failure, returns a
    note describing what went wrong instead, so the reply can explain rather
    than go silent.
    """
    label = "document" if media_type == "document" else "photo"

    if file_length and file_length > MAX_MEDIA_BYTES:
        note = (f"[The user sent a {label} ({file_length} bytes), but it exceeds "
                 f"the {MAX_MEDIA_BYTES}-byte limit, so it was not downloaded.]")
        return f"{note} {caption}".strip()

    result = download_media(msg_id, chat_jid)
    if not result.get("success"):
        note = (f"[The user sent a {label} but it could not be downloaded: "
                 f"{result.get('message', 'unknown error')}]")
        return f"{note} {caption}".strip()

    src = Path(result["path"])
    try:
        actual_size = src.stat().st_size
    except OSError as e:
        note = f"[The user sent a {label} but it could not be read after download: {e}]"
        return f"{note} {caption}".strip()

    if actual_size > MAX_MEDIA_BYTES:
        log(f"Downloaded {label} {src} is {actual_size} bytes, over the "
            f"{MAX_MEDIA_BYTES}-byte cap; discarding.")
        note = (f"[The user sent a {label} that turned out to be {actual_size} "
                 f"bytes, over the {MAX_MEDIA_BYTES}-byte limit, so it was discarded.]")
        return f"{note} {caption}".strip()

    chat_dir = UPLOAD_DIR / sanitize_path_component(chat_jid, 80, keep_dots=False)
    chat_dir.mkdir(parents=True, exist_ok=True)
    raw_name = Path(filename or result.get("filename") or "file").name
    safe_name = sanitize_path_component(raw_name, 100, keep_dots=True)
    dest = chat_dir / f"{sanitize_path_component(msg_id, 40, keep_dots=False)}_{safe_name}"

    try:
        shutil.copy2(src, dest)
    except OSError as e:
        note = f"[The user sent a {label} but it could not be saved for reading: {e}]"
        return f"{note} {caption}".strip()

    note = f"[{label.capitalize()} received: {dest.relative_to(UPLOAD_DIR)}]"
    return f"{note} {caption}".strip()


def cleanup_expired_uploads() -> None:
    if not UPLOAD_DIR.exists():
        return
    cutoff = time.time() - UPLOAD_MAX_AGE_DAYS * 86400
    removed = 0
    for path in UPLOAD_DIR.rglob("*"):
        if path.is_file():
            try:
                if path.stat().st_mtime < cutoff:
                    path.unlink()
                    removed += 1
            except OSError:
                continue
    for sub in sorted(UPLOAD_DIR.rglob("*"), reverse=True):
        if sub.is_dir():
            try:
                sub.rmdir()  # only succeeds if already empty
            except OSError:
                pass
    if removed:
        log(f"Deleted {removed} expired upload(s) older than {UPLOAD_MAX_AGE_DAYS} days.")


def fetch_new_messages(state):
    conn = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True, timeout=5)
    try:
        cur = conn.cursor()
        cur.execute(
            """SELECT id, chat_jid, sender, content, timestamp, media_type, filename, file_length
               FROM messages
               WHERE is_from_me = 0 AND timestamp >= ?
               ORDER BY timestamp ASC""",
            (state["last_timestamp"],),
        )
        rows = cur.fetchall()
    finally:
        conn.close()

    already_seen = set(state["last_timestamp_ids"])
    fresh = [
        r for r in rows
        if not (r[4] == state["last_timestamp"] and r[0] in already_seen)
    ]

    if rows:
        max_ts = max(r[4] for r in rows)
        state["last_timestamp"] = max_ts
        state["last_timestamp_ids"] = [r[0] for r in rows if r[4] == max_ts]

    return fresh


def ask_funes(session: str, message: str) -> str | None:
    payload = json.dumps({"agent": AGENT_NAME, "session": session, "message": message}).encode()
    req = urllib.request.Request(
        f"{FUNES_API_URL}/api/chat", data=payload,
        headers={"Content-Type": "application/json"}, method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=CHAT_TIMEOUT) as resp:
            event_type, text = None, None
            for raw_line in resp:
                line = raw_line.decode("utf-8", "replace").rstrip("\n")
                if line.startswith("event: "):
                    event_type = line[len("event: "):]
                elif line.startswith("data: "):
                    data = json.loads(line[len("data: "):])
                    if event_type == "done":
                        text = data.get("text", "")
                    elif event_type == "error":
                        log(f"Funes agent error: {data.get('message')}")
                        return None
            return text
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
        log(f"Failed to reach Funes /api/chat: {e}")
        return None


def send_reply(chat_jid: str, text: str) -> bool:
    payload = json.dumps({"recipient": chat_jid, "message": text}).encode()
    req = urllib.request.Request(
        f"{BRIDGE_API_URL}/api/send", data=payload,
        headers={"Content-Type": "application/json"}, method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            result = json.loads(resp.read().decode())
            if not result.get("success", True):
                log(f"Bridge refused send to {chat_jid}: {result}")
                return False
            return True
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
        log(f"Failed to send reply via bridge: {e}")
        return False


def main():
    if not WHITELIST:
        log("WHATSAPP_WHITELIST is empty — no chat_jid is allowed to trigger "
            "an auto-reply. Set it in config/funes.local. Exiting.")
        sys.exit(1)

    log(f"Watching {DB_PATH} for messages from {len(WHITELIST)} whitelisted chat(s), "
        f"polling every {POLL_SECONDS}s.")
    state = load_state()
    state.setdefault("last_cleanup_ts", 0)

    while True:
        try:
            new_messages = fetch_new_messages(state)
        except sqlite3.OperationalError as e:
            log(f"DB busy, will retry next cycle: {e}")
            time.sleep(POLL_SECONDS)
            continue

        for msg_id, chat_jid, sender, content, timestamp, media_type, filename, file_length in new_messages:
            if chat_jid not in WHITELIST:
                continue

            message_text = (content or "").strip()
            if media_type in ("document", "image"):
                message_text = handle_incoming_media(
                    msg_id, chat_jid, media_type, filename, file_length, message_text)
            elif not message_text:
                continue  # other media types or empty message, nothing to reply to

            if not message_text:
                continue

            log(f"New message from {sender} in {chat_jid}: {message_text[:80]!r}")
            reply = ask_funes(sanitize_session(chat_jid), message_text)
            if reply is None or not reply.strip():
                log("No reply generated, skipping send.")
                continue
            if send_reply(chat_jid, reply):
                log(f"Replied in {chat_jid}: {reply[:80]!r}")

        if time.time() - state["last_cleanup_ts"] > CLEANUP_INTERVAL_SECONDS:
            cleanup_expired_uploads()
            state["last_cleanup_ts"] = time.time()

        save_state(state)
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
