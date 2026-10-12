#!/usr/bin/env python3
"""
post_tweet.py — Posts one item of today's issue to LinkedIn.

Usage:
    python3 post_tweet.py <post_number> [--dir PATH] [--publication ID]
                          [--date YYYY-MM-DD] [--dry-run]

If <dir>/images/<publication>/<date>/<post_number>.png exists (see
generate_images.py) it is attached to the post. An image is garnish: a missing
file, or an upload that fails, posts text-only rather than not at all.

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


# ── The illustration ──────────────────────────────────────────────────────────

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def image_path(work_dir: Path, publication: str, target_date: date, slot: int):
    """The slot's illustration, or None. generate_images.py writes
    <dir>/images/<publication>/<date>/<slot>.png; anything that is not a real PNG
    (empty, truncated at the header, an error page) counts as absent, so a
    render that died half way becomes a text-only post rather than a broken
    attachment."""
    path = (work_dir / "images" / publication / target_date.isoformat()
            / f"{slot}.png")
    try:
        with open(path, "rb") as f:
            return path if f.read(len(PNG_MAGIC)) == PNG_MAGIC else None
    except OSError:
        return None


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


def upload_image(path: Path, token: str, author: str) -> str:
    """Registers an upload, PUTs the bytes, returns the asset URN."""
    auth = {"Authorization": f"Bearer {token}"}
    r = requests.post(
        "https://api.linkedin.com/v2/assets?action=registerUpload",
        headers={**auth, "Content-Type": "application/json"},
        json={"registerUploadRequest": {
            "recipes": ["urn:li:digitalmediaRecipe:feedshare-image"],
            "owner": author,
            "serviceRelationships": [{
                "relationshipType": "OWNER",
                "identifier": "urn:li:userGeneratedContent"}]}})
    r.raise_for_status()
    value = r.json()["value"]
    upload_url = (value["uploadMechanism"]
                  ["com.linkedin.digitalmedia.uploading.MediaUploadHttpRequest"]
                  ["uploadUrl"])
    put = requests.put(upload_url, headers=auth, data=path.read_bytes())
    put.raise_for_status()
    return value["asset"]


def post_to_linkedin(text: str, token: str, image=None, title: str = "") -> str:
    """Posts `text`, with `image` attached when there is one and it uploads.

    Only the *upload* falls back to text-only. A failure creating the post
    itself is raised and never retried: it may have half gone through, and
    retrying is how a profile ends up with the same item twice."""
    author = get_person_urn(token)

    asset = None
    if image is not None:
        try:
            asset = upload_image(image, token, author)
        except Exception as e:                       # noqa: BLE001 — reported below
            print(f"WARNING: image upload failed ({e}); posting text-only.")

    content = {"shareCommentary": {"text": text}, "shareMediaCategory": "NONE"}
    if asset:
        media = {"status": "READY", "media": asset}
        if title:
            media["title"] = {"text": title}
        content.update(shareMediaCategory="IMAGE", media=[media])

    r = requests.post(
        "https://api.linkedin.com/v2/ugcPosts",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "X-Restli-Protocol-Version": "2.0.0"
        },
        json={
            "author": author,
            "lifecycleState": "PUBLISHED",
            "specificContent": {"com.linkedin.ugc.ShareContent": content},
            "visibility": {
                "com.linkedin.ugc.MemberNetworkVisibility": "PUBLIC"
            }
        }
    )
    r.raise_for_status()
    return r.headers.get("x-restli-id", r.text)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    return main_with_args(sys.argv[1:])


# Split from main() so the exit codes can be tested without patching sys.argv —
# they are the interface cron branches on, so they deserve coverage.
def main_with_args(args):

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
    except RuntimeError as e:
        print(f"REFUSED: {e}")
        return 3
    except (KeyError, json.JSONDecodeError) as e:
        print(f"ERROR: {e}")
        return 1

    # An issue can legitimately carry fewer items than there are cron slots:
    # publish_issue drops an item whose post text no page in the pool supports
    # rather than sinking the whole issue. A slot past the end is therefore an
    # expected quiet day, not a failure — exiting non-zero here would put an
    # ERROR in the log most days and teach us to stop reading it.
    item_count = len(issue.get("items", []))
    if post_number > item_count:
        print(f"Nothing to post: today's {publication} issue has {item_count} "
              f"item(s), and this is slot {post_number}.")
        return 0

    try:
        text = post_body(issue, post_number)
    except (IndexError, KeyError) as e:
        print(f"ERROR: {e}")
        return 1

    image = image_path(work_dir, publication, target_date, post_number)
    title = issue["items"][post_number - 1].get("headline", "")
    print(f"Posting #{post_number} to LinkedIn:\n{text}\n")
    print(f"image: {image}" if image else "no image for this slot (text-only)")
    if dry_run:
        print("[DRY RUN] nothing was posted.")
        return 0

    token = get_valid_token(load_env(work_dir / ".env"))
    print(f"Posted successfully! Post ID: {post_to_linkedin(text, token, image, title)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
