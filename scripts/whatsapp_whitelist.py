#!/usr/bin/env python3
"""Look up chats on the dedicated Funes WhatsApp number and manage
WHATSAPP_WHITELIST in config/funes.local, without hand-writing sqlite or
touching the file yourself.

    python3 scripts/whatsapp_whitelist.py list
    python3 scripts/whatsapp_whitelist.py add <name-or-jid-substring>
    python3 scripts/whatsapp_whitelist.py remove <name-or-jid-substring>

This only edits config/funes.local — it does not restart
whatsapp-autoresponder.service, so changes take effect on the next restart.

Deliberately a local, human-run CLI rather than something exposed to an
agent: the whitelist is the one thing standing between "replies only trusted
contacts" and "replies anyone" (see scripts/whatsapp_autoresponder.py), so
changing it should stay a step only the operator can take.
"""
import re
import sqlite3
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FUNES_LOCAL = REPO_ROOT / "config" / "funes.local"
WHITELIST_KEY = "WHATSAPP_WHITELIST"


def load_config() -> dict:
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
    return cfg


CFG = load_config()
DB_PATH = CFG.get(
    "WHATSAPP_DB_PATH",
    str(REPO_ROOT / "third-party/whatsapp-mcp/whatsapp-bridge/store/messages.db"),
)


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def load_chats() -> list[tuple[str, str]]:
    db_path = Path(DB_PATH)
    if not db_path.exists():
        die(f"no message store at {db_path} — has the dedicated bridge instance "
            f"been paired yet? (see scripts/whatsapp-bridge-funes.service)")
    conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True, timeout=5)
    try:
        return conn.execute("SELECT jid, COALESCE(name, '') FROM chats ORDER BY name").fetchall()
    finally:
        conn.close()


def current_whitelist() -> list[str]:
    return [jid.strip() for jid in CFG.get(WHITELIST_KEY, "").split(",") if jid.strip()]


def write_whitelist(jids: list[str]) -> None:
    value = ",".join(jids)
    line = f"{WHITELIST_KEY}={value}\n"
    if not FUNES_LOCAL.exists():
        FUNES_LOCAL.write_text(line)
        return

    lines = FUNES_LOCAL.read_text().splitlines(keepends=True)
    pattern = re.compile(rf"^{WHITELIST_KEY}\s*=")
    for i, existing in enumerate(lines):
        if pattern.match(existing.strip()):
            lines[i] = line
            FUNES_LOCAL.write_text("".join(lines))
            return

    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    lines.append(line)
    FUNES_LOCAL.write_text("".join(lines))


def find_matches(query: str, rows: list[tuple[str, str]]) -> list[tuple[str, str]]:
    q = query.lower()
    return [(jid, name) for jid, name in rows if q in jid.lower() or q in name.lower()]


def cmd_list() -> None:
    whitelisted = set(current_whitelist())
    for jid, name in load_chats():
        marker = " [whitelisted]" if jid in whitelisted else ""
        print(f"{jid}\t{name}{marker}")


def cmd_add(query: str, allow_group: bool) -> None:
    matches = find_matches(query, load_chats())
    if not matches:
        die(f"no chat matches {query!r} — try `list` to browse")
    if len(matches) > 1:
        print(f"{len(matches)} chats match {query!r} — be more specific:", file=sys.stderr)
        for jid, name in matches:
            print(f"  {jid}\t{name}", file=sys.stderr)
        sys.exit(1)

    jid, name = matches[0]
    if jid.endswith("@g.us") and not allow_group:
        die(f"{jid} ({name}) is a GROUP chat — whitelisting it means auto-replying "
            f"to every message anyone posts there. Re-run with --allow-group if "
            f"that's really what you want.")

    whitelist = current_whitelist()
    if jid in whitelist:
        print(f"{jid} ({name}) is already whitelisted.")
        return

    whitelist.append(jid)
    write_whitelist(whitelist)
    print(f"Added {jid} ({name}). Restart whatsapp-autoresponder.service to apply.")


def cmd_remove(query: str) -> None:
    whitelist = current_whitelist()
    if not whitelist:
        die("whitelist is already empty")

    chats_by_jid = {jid: name for jid, name in load_chats()} if Path(DB_PATH).exists() else {}
    matches = [jid for jid in whitelist
               if query.lower() in jid.lower() or query.lower() in chats_by_jid.get(jid, "").lower()]

    if not matches:
        die(f"no whitelisted chat matches {query!r} — try `list` to see current entries")
    if len(matches) > 1:
        print(f"{len(matches)} whitelisted chats match {query!r} — be more specific:", file=sys.stderr)
        for jid in matches:
            print(f"  {jid}\t{chats_by_jid.get(jid, '')}", file=sys.stderr)
        sys.exit(1)

    jid = matches[0]
    whitelist.remove(jid)
    write_whitelist(whitelist)
    print(f"Removed {jid}. Restart whatsapp-autoresponder.service to apply.")


def main() -> None:
    args = sys.argv[1:]
    if not args or args[0] not in ("list", "add", "remove"):
        print(__doc__)
        sys.exit(0 if not args else 1)

    command = args[0]
    if command == "list":
        cmd_list()
    elif command == "add":
        allow_group = "--allow-group" in args[1:]
        rest = [a for a in args[1:] if a != "--allow-group"]
        if not rest:
            die("usage: whatsapp_whitelist.py add <name-or-jid-substring> [--allow-group]")
        cmd_add(" ".join(rest), allow_group)
    elif command == "remove":
        if len(args) < 2:
            die("usage: whatsapp_whitelist.py remove <name-or-jid-substring>")
        cmd_remove(" ".join(args[1:]))


if __name__ == "__main__":
    main()
