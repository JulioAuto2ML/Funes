#!/usr/bin/env python3
"""
send_newsletter.py — Sends the daily AI Pulse HTML newsletter via Gmail SMTP.

Usage:
    python3 send_newsletter.py [YYYY-MM-DD] [--dir PATH] [--dry-run]
                               [--subject TEXT] [--recipients FILE]

    If no date is given, today's date is used. --subject and --recipients come
    from the publication config via publish_issue.py; on their own the defaults
    are the AI newsletter's, which is what this script was written for.

The issue directory — holding newsletter_<date>.html, subscribers.txt and
optionally .env — is `--dir PATH`, else $FUNES_PUBLISH_DIR, else the current
working directory. This script lives in the Funes repo and is deployed by
`git pull`; the mailing list and the credentials stay on the host and are
never in the repo.

Credentials — GMAIL_ADDRESS and GMAIL_APP_PASSWORD — are read from the
process environment first (Funes populates this from config/funes.local at
startup, see core/funes_config.h), with <dir>/.env, if present, overriding
that. A Gmail App Password is NOT your regular password — generate one at:
https://myaccount.google.com/apppasswords (requires 2FA on your Google
account).

Subscribers are read from <dir>/subscribers.txt (one address per line; # = comment).
"""

import os
import re
import sys
import smtplib
from pathlib import Path
from datetime import date
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

# ── Helpers ────────────────────────────────────────────────────────────────────

def issue_dir(explicit=None) -> Path:
    """Where the issue, the subscriber list and .env live."""
    return Path(explicit or os.environ.get("FUNES_PUBLISH_DIR") or Path.cwd()).resolve()


def load_env(path: Path) -> dict:
    env = {}
    if not path.exists():
        return env
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, value = line.partition("=")
            env[key.strip()] = value.strip()
    return env


def load_subscribers(path: Path) -> list[str]:
    if not path.exists():
        print(f"WARNING: subscribers file not found at {path}")
        return []
    addrs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            addrs.append(line)
    return addrs


def load_newsletter(work_dir: Path, target_date: date,
                    subject: str = "") -> tuple[str, str]:
    """Returns (subject, html_body). Raises FileNotFoundError if missing."""
    html_path = work_dir / f"newsletter_{target_date.isoformat()}.html"
    if not html_path.exists():
        raise FileNotFoundError(f"Newsletter not found: {html_path}")
    html = html_path.read_text(encoding="utf-8")

    if not subject:
        # The AI newsletter's line, kept as the default so a publication with no
        # configured subject behaves exactly as this script always has.
        friendly = target_date.strftime("%A, %B %-d, %Y")
        subject = f"AI Pulse · {friendly}"
    return subject, html


def send_newsletter(
    gmail_address: str,
    app_password: str,
    subscribers: list[str],
    subject: str,
    html_body: str,
    dry_run: bool = False,
    sender_name: str = "AI Pulse",
) -> None:
    """Sends the newsletter to every subscriber via Gmail SMTP."""

    if not subscribers:
        print("No subscribers found — nothing to send.")
        return

    print(f"Connecting to Gmail SMTP as {gmail_address} …")
    with smtplib.SMTP("smtp.gmail.com", 587) as server:
        server.ehlo()
        server.starttls()
        server.login(gmail_address, app_password)
        print("Authenticated ✓")

        for addr in subscribers:
            msg = MIMEMultipart("alternative")
            msg["Subject"] = subject
            # The subject already names the publication; the From line follows
            # it rather than hardcoding one publication's name into the sender.
            msg["From"]    = f"{sender_name} <{gmail_address}>"
            msg["To"]      = addr

            # Plain-text fallback (strip tags)
            plain = re.sub(r"<[^>]+>", "", html_body)
            plain = re.sub(r"\n{3,}", "\n\n", plain).strip()
            msg.attach(MIMEText(plain, "plain", "utf-8"))
            msg.attach(MIMEText(html_body, "html", "utf-8"))

            if dry_run:
                print(f"  [DRY RUN] Would send to: {addr}")
            else:
                server.sendmail(gmail_address, addr, msg.as_string())
                print(f"  ✓ Sent to {addr}")

    print("Done.")


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]

    def take(flag):
        if flag not in args:
            return None
        i = args.index(flag)
        if i + 1 >= len(args):
            print(f"ERROR: {flag} needs a value")
            sys.exit(1)
        value = args[i + 1]
        del args[i:i + 2]
        return value

    explicit_dir = take("--dir")
    subject_line = take("--subject") or ""
    recipients   = take("--recipients") or "subscribers.txt"

    dry_run = "--dry-run" in args
    positional = [a for a in args if not a.startswith("-")]

    if positional:
        try:
            target_date = date.fromisoformat(positional[0])
        except ValueError:
            print(f"ERROR: invalid date '{positional[0]}' — use YYYY-MM-DD format")
            sys.exit(1)
    else:
        target_date = date.today()

    work_dir = issue_dir(explicit_dir)

    # Load credentials — process environment first (Funes sets this from
    # config/funes.local at startup), <dir>/.env overrides if present.
    creds = {**os.environ, **load_env(work_dir / ".env")}
    gmail_address = creds.get("GMAIL_ADDRESS", "").strip()
    app_password  = creds.get("GMAIL_APP_PASSWORD", "").strip()

    if not gmail_address or not app_password:
        print(
            f"ERROR: GMAIL_ADDRESS and GMAIL_APP_PASSWORD must be set, either "
            f"in the environment (e.g. config/funes.local) or in "
            f"{work_dir / '.env'}\n"
            "  Generate an App Password at: https://myaccount.google.com/apppasswords\n"
            "  (2FA must be enabled on your Google account)"
        )
        sys.exit(1)

    # Load subscribers
    subscribers = load_subscribers(work_dir / recipients)
    print(f"Subscribers loaded: {len(subscribers)}")

    # Load newsletter
    try:
        subject, html_body = load_newsletter(work_dir, target_date, subject_line)
    except FileNotFoundError as e:
        print(f"ERROR: {e}")
        sys.exit(1)

    print(f"Subject: {subject}")
    print(f"Sending to {len(subscribers)} subscriber(s) …\n")

    # "AI Pulse · Thursday, ..." -> "AI Pulse". A subject built from a title is
    # the closest thing to a publication name this script is given.
    sender_name = subject.split("·")[0].strip() or "AI Pulse"
    send_newsletter(gmail_address, app_password, subscribers, subject, html_body,
                    dry_run=dry_run, sender_name=sender_name)


if __name__ == "__main__":
    main()
