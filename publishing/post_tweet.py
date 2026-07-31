#!/usr/bin/env python3
"""
post_tweet.py — Posts one item of today's issue to LinkedIn.

Usage:
    python3 post_tweet.py <post_number> [--dir PATH] [--publication ID]
                          [--date YYYY-MM-DD] [--dry-run]

    post_number: 1-based index of the item to publish.

Two things changed when this moved into the repo, and both were bugs.

**It reads the issue JSON, not the posts .txt.** There used to be two
hand-rolled parsers of an undocumented format that a language model typed out —
one here, one in publish_newsletter.py — and they did not agree with each other.
Now the issue JSON is the source of truth and the .txt is a generated artifact,
so the URL this posts is the one the harvester fetched and the publisher
link-checked, not one re-parsed out of prose.

**It refuses to post when the day's run says nothing was sent.** Cron fires ten
times a day whether or not the morning's newsletter succeeded. Without this
check, a blocked run left yesterday's posts file in place and cron cheerfully
published from it — the asymmetry that made a silent failure into ten public
ones.

Credentials are read from <dir>/.env, which stays on the host.

Exit codes:
    0  posted (or, with --dry-run, would have been)
    1  usage/input error
    3  refused: no run record for that date, or the issue was not sent
"""

import json
import sys
import time
from datetime import date
from pathlib import Path

import requests

SCRIPT_DIR = Path(__file__).parent.resolve()

sys.path.insert(0, str(SCRIPT_DIR))
from publish_newsletter import issue_dir           # noqa: E402


# ── The issue and its run record ──────────────────────────────────────────────

def load_sent_issue(work_dir: Path, publication: str, target_date: date) -> dict:
    """The issue, but only if the run record says it went out.

    Raises RuntimeError with a message written for a cron log — nobody is
    watching when this fires, so the line it leaves behind is the whole
    diagnosis.
    """
    day = target_date.isoformat()
    run_path = work_dir / "runs" / publication / f"{day}.json"
    if not run_path.exists():
        raise RuntimeError(
            f"no run record at {run_path} — {publication} was never published "
            f"for {day}, so there is nothing to post.")

    record = json.loads(run_path.read_text(encoding="utf-8"))
    status = record.get("status")
    if status != "sent":
        raise RuntimeError(
            f"{publication} {day} has status '{status}'"
            + (f" ({record['reason']})" if record.get("reason") else "")
            + " — refusing to post from an issue that did not go out.")

    issue_path = work_dir / "issues" / publication / f"{day}.json"
    if not issue_path.exists():
        raise RuntimeError(f"run record says sent, but no issue at {issue_path}")
    return json.loads(issue_path.read_text(encoding="utf-8"))


def post_body(issue: dict, post_number: int) -> str:
    """Item N as a LinkedIn post: the text, the link, and the publication's own
    call to action. Every part of it is looked up, not parsed."""
    items = issue.get("items", [])
    if post_number < 1 or post_number > len(items):
        raise IndexError(f"post_number must be between 1 and {len(items)}")

    item = items[post_number - 1]
    emoji = item.get("emoji", "").strip()
    text = f"{emoji} {item['text']}".strip()
    body = f"{text}\n{item['url']}"
    cta = (issue.get("cta") or "").strip()
    return f"{body}\n\n{cta}" if cta else body


# ── Credentials ───────────────────────────────────────────────────────────────

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


def get_valid_token(creds: dict) -> str:
    token = creds.get("LI_ACCESS_TOKEN", "")
    expiry = int(creds.get("LI_TOKEN_EXPIRY", 0))

    if not token:
        print("ERROR: no access token found. Run:  python3 linkedin_auth.py")
        sys.exit(1)

    if expiry and time.time() > expiry:
        print("ERROR: LinkedIn access token has expired (valid for 60 days).")
        print("Re-run:  python3 linkedin_auth.py")
        sys.exit(1)

    if expiry and time.time() > expiry - 7 * 86400:
        days_left = int((expiry - time.time()) / 86400)
        print(f"Warning: LinkedIn token expires in {days_left} day(s). "
              "Run linkedin_auth.py soon to avoid interruption.")

    return token


# ── LinkedIn API ──────────────────────────────────────────────────────────────

def get_person_urn(token: str) -> str:
    r = requests.get(
        "https://api.linkedin.com/v2/userinfo",
        headers={"Authorization": f"Bearer {token}"}
    )
    r.raise_for_status()
    return f"urn:li:person:{r.json()['sub']}"


def post_to_linkedin(text: str, token: str) -> str:
    payload = {
        "author": get_person_urn(token),
        "lifecycleState": "PUBLISHED",
        "specificContent": {
            "com.linkedin.ugc.ShareContent": {
                "shareCommentary": {"text": text},
                "shareMediaCategory": "NONE"
            }
        },
        "visibility": {
            "com.linkedin.ugc.MemberNetworkVisibility": "PUBLIC"
        }
    }
    r = requests.post(
        "https://api.linkedin.com/v2/ugcPosts",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "X-Restli-Protocol-Version": "2.0.0"
        },
        json=payload
    )
    r.raise_for_status()
    return r.headers.get("x-restli-id", r.text)


# ── Main ──────────────────────────────────────────────────────────────────────

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
    publication = take("--publication") or "ai-pulse"
    day = take("--date")
    dry_run = "--dry-run" in args

    positional = [a for a in args if not a.startswith("-")]
    if len(positional) != 1 or not positional[0].isdigit():
        print("Usage: python3 post_tweet.py <post_number> [--dir PATH] "
              "[--publication ID] [--date YYYY-MM-DD] [--dry-run]")
        return 1
    post_number = int(positional[0])

    try:
        target_date = date.fromisoformat(day) if day else date.today()
    except ValueError:
        print(f"ERROR: invalid date '{day}' — use YYYY-MM-DD")
        return 1

    work_dir = issue_dir(explicit_dir)

    try:
        issue = load_sent_issue(work_dir, publication, target_date)
        text = post_body(issue, post_number)
    except RuntimeError as e:
        print(f"REFUSED: {e}")
        return 3
    except (IndexError, KeyError, json.JSONDecodeError) as e:
        print(f"ERROR: {e}")
        return 1

    print(f"Posting #{post_number} to LinkedIn:\n{text}\n")
    if dry_run:
        print("[DRY RUN] nothing was posted.")
        return 0

    token = get_valid_token(load_env(work_dir / ".env"))
    print(f"Posted successfully! Post ID: {post_to_linkedin(text, token)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
