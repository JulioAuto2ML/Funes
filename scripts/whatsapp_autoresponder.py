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
bridge's /api/download and copied into the sending contact's own Funes
workspace ($FUNES_WORKSPACE_DIR/<user_id>/whatsapp-uploads/), where the
whatsapp-autoresponder agent's `read_file` tool (scoped to that directory
only, see workspace_dir in its yaml) can read them — PDFs and plain text via
text extraction, images via the vision-capable backend if one is configured.
Files older than WHATSAPP_UPLOAD_MAX_AGE_DAYS are deleted automatically.
Audio and video attachments are still ignored.

A contact can send RESET_COMMAND ("/new") to start a fresh conversation.
This rotates that chat_jid to a new session (see sanitize_session) rather
than deleting the old one's history — the same "start a new session, leave
the old one intact" behavior the web UI's own "New Chat" button has, just
triggered over WhatsApp. Handled here directly, in plain Python, without
ever going through Funes/the LLM.
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
# Shared secret that lets this script authenticate to Funes 4.0's API. Empty
# means the calls go out unauthenticated, which only works against a pre-4.0
# server — 4.0 answers 401 and ask_funes() logs what to fix.
SERVICE_TOKEN = CFG.get("FUNES_SERVICE_TOKEN", "")
POLL_SECONDS = float(CFG.get("WHATSAPP_POLL_SECONDS", "5"))
CHAT_TIMEOUT = float(CFG.get("WHATSAPP_CHAT_TIMEOUT_SECONDS", "120"))
WHITELIST = {
    jid.strip() for jid in CFG.get("WHATSAPP_WHITELIST", "").split(",") if jid.strip()
}

# Funes 4.0 workspaces are per user: <root>/<user_id>/. Uploads have to land
# inside the sending contact's own workspace, because that is the only place
# the whatsapp-autoresponder agent's read_file is allowed to look (its
# `workspace_dir: whatsapp-uploads` in the yaml resolves relative to it).
WORKSPACE_ROOT = Path(os.path.expanduser(
    CFG.get("FUNES_WORKSPACE_DIR", "~/.funes/workspace")))
# The folder name inside each user's workspace. Must match `workspace_dir` in
# agents/whatsapp-autoresponder.yaml.
UPLOAD_SUBDIR = CFG.get("WHATSAPP_UPLOAD_SUBDIR", "whatsapp-uploads")
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


RESET_COMMAND = "/new"


def sanitize_session(chat_jid: str, generation: int = 0) -> str:
    """Session name for a chat, optionally a later "generation" after a
    RESET_COMMAND — matches the web UI's own "New Chat" behavior (a fresh
    session id, the previous one left intact rather than deleted) so a
    contact can start over without wiping anything.
    """
    base = "whatsapp_" + re.sub(r"[^A-Za-z0-9_-]", "_", chat_jid)
    session = base if generation <= 0 else f"{base}_g{generation}"
    return session[:64]


def session_generation(state: dict, chat_jid: str) -> int:
    return state.get("session_generations", {}).get(chat_jid, 0)


def bump_session_generation(state: dict, chat_jid: str) -> int:
    generations = state.setdefault("session_generations", {})
    generations[chat_jid] = generations.get(chat_jid, 0) + 1
    return generations[chat_jid]


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
                           file_length: int, caption: str, user_id: int) -> str:
    """Download a WhatsApp "document" or "image" attachment and copy it into
    the sending contact's own Funes workspace, returning the text to hand to
    Funes (a `[Document received:
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

    upload_dir = upload_dir_for(user_id)
    chat_dir = upload_dir / sanitize_path_component(chat_jid, 80, keep_dots=False)
    chat_dir.mkdir(parents=True, exist_ok=True)
    raw_name = Path(filename or result.get("filename") or "file").name
    safe_name = sanitize_path_component(raw_name, 100, keep_dots=True)
    dest = chat_dir / f"{sanitize_path_component(msg_id, 40, keep_dots=False)}_{safe_name}"

    try:
        shutil.copy2(src, dest)
    except OSError as e:
        note = f"[The user sent a {label} but it could not be saved for reading: {e}]"
        return f"{note} {caption}".strip()

    note = f"[{label.capitalize()} received: {dest.relative_to(upload_dir)}]"
    return f"{note} {caption}".strip()


def cleanup_expired_uploads() -> None:
    """Sweep every user's upload folder, not just one: uploads now live under
    <workspace>/<user_id>/<UPLOAD_SUBDIR>/, so a single directory walk would
    silently stop expiring files for everyone but the first account."""
    if not WORKSPACE_ROOT.exists():
        return
    cutoff = time.time() - UPLOAD_MAX_AGE_DAYS * 86400
    removed = 0
    upload_dirs = [d for d in WORKSPACE_ROOT.glob(f"*/{UPLOAD_SUBDIR}") if d.is_dir()]
    for upload_dir in upload_dirs:
        for path in upload_dir.rglob("*"):
            if path.is_file():
                try:
                    if path.stat().st_mtime < cutoff:
                        path.unlink()
                        removed += 1
                except OSError:
                    continue
        for sub in sorted(upload_dir.rglob("*"), reverse=True):
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


# chat_jid -> user_id, resolved by Funes (never guessed here). Cached because
# it only changes when an admin runs `funes jid-map`, and a failed lookup is
# not cached so a mapping added later is picked up without a restart.
_USER_ID_CACHE: dict[str, int] = {}


def resolve_user_id(chat_jid: str) -> int | None:
    """Ask Funes which user this WhatsApp number acts as.

    Identity resolution stays server-side: this sends the jid and reads back
    the id Funes assigned it. Returning None means the number is not mapped,
    which is also what makes /api/chat refuse the message.
    """
    if chat_jid in _USER_ID_CACHE:
        return _USER_ID_CACHE[chat_jid]
    if not SERVICE_TOKEN:
        return None
    req = urllib.request.Request(
        f"{FUNES_API_URL}/api/auth/status",
        headers={"X-Funes-Service-Token": SERVICE_TOKEN,
                 "X-Funes-User-Jid": chat_jid},
        method="GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8", "replace"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
        log(f"Could not resolve a Funes user for {chat_jid}: {e}")
        return None

    user = data.get("user") or {}
    uid = user.get("id")
    if not isinstance(uid, int):
        log(f"WhatsApp number {chat_jid} is not mapped to a Funes user. "
            f"Map it with: funes jid-map {chat_jid} <username>")
        return None
    _USER_ID_CACHE[chat_jid] = uid
    return uid


def upload_dir_for(user_id: int) -> Path:
    return WORKSPACE_ROOT / str(user_id) / UPLOAD_SUBDIR


def ask_funes(session: str, message: str, chat_jid: str) -> str | None:
    payload = json.dumps({"agent": AGENT_NAME, "session": session, "message": message}).encode()
    headers = {"Content-Type": "application/json"}
    # Funes 4.0 authenticates every /api/ call. This script is a service, not a
    # browser: the token says the caller is trusted, and the jid says who the
    # request is for. Funes maps that jid to a user (see `funes jid-map`) and
    # refuses the call outright if it maps to nobody — identity is resolved
    # there, not here, so this script can never talk its way into another
    # user's memories by naming a different number.
    if SERVICE_TOKEN:
        headers["X-Funes-Service-Token"] = SERVICE_TOKEN
        headers["X-Funes-User-Jid"] = chat_jid
    req = urllib.request.Request(
        f"{FUNES_API_URL}/api/chat", data=payload,
        headers=headers, method="POST",
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
    except urllib.error.HTTPError as e:
        # 401/403 here is a configuration fault, not a bad message, and it will
        # hit every single incoming WhatsApp message until someone fixes it.
        # Say exactly which of the two setup steps is missing rather than
        # logging a bare status code nobody can act on.
        if e.code in (401, 403):
            log(f"Funes rejected the service call ({e.code}) for jid {chat_jid}. "
                "Check FUNES_SERVICE_TOKEN matches the server's, and that this "
                f"number is mapped: funes jid-map {chat_jid} <username>")
        else:
            log(f"Funes /api/chat returned HTTP {e.code}")
        return None
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

            # Handled directly, never forwarded to Funes: this is a
            # mechanical session-rotation, not something needing model
            # judgment (see the project's deterministic-over-agentic rule).
            if not media_type and message_text.lower() == RESET_COMMAND:
                bump_session_generation(state, chat_jid)
                save_state(state)
                log(f"Reset session for {chat_jid} (new generation "
                    f"{session_generation(state, chat_jid)}).")
                send_reply(chat_jid, "Starting a fresh conversation — I won't recall this "
                                      "thread's earlier messages, but everything else I "
                                      "remember about you is still there.")
                continue

            if media_type in ("document", "image"):
                # Resolve who this number is before writing anything: the file
                # has to land in that user's workspace, and an unmapped number
                # has no workspace to write into (and /api/chat would refuse
                # the message anyway).
                user_id = resolve_user_id(chat_jid)
                if user_id is None:
                    log(f"Ignoring {media_type} from unmapped number {chat_jid}.")
                    continue
                message_text = handle_incoming_media(
                    msg_id, chat_jid, media_type, filename, file_length,
                    message_text, user_id)
            elif not message_text:
                continue  # other media types or empty message, nothing to reply to

            if not message_text:
                continue

            log(f"New message from {sender} in {chat_jid}: {message_text[:80]!r}")
            session = sanitize_session(chat_jid, session_generation(state, chat_jid))
            reply = ask_funes(session, message_text, chat_jid)
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
