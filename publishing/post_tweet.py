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

**The link goes in the first comment, not the body.** LinkedIn demotes a post
that carries an outbound link, so the post is the text (and image) alone and the
source URL follows as a comment, in a second API call. That makes a post two
steps, and the second can fail after the first has made something public — so
the post's id is written to <dir>/posted/<publication>/<date>.json the moment
LinkedIn returns it, before the comment is attempted. Re-running a slot then
only ever finishes the comment: it can never repost. Every invocation also
sweeps today's earlier slots for comments still owed, so a transient failure is
repaired by the next cron slot without anyone looking.

Credentials are read from <dir>/.env, which stays on the host.

Exit codes:
    0  posted and commented (or, with --dry-run, would have been)
    1  usage/input error
    3  refused: no run record for that date, or the issue was not sent
    4  posted, but a link comment could not be added (post id is on record;
       re-run the slot, or wait for the next one, to finish it)
"""

import json
import os
import sys
import time
from datetime import date
from pathlib import Path
from urllib.parse import quote

import requests

SCRIPT_DIR = Path(__file__).parent.resolve()

EXIT_COMMENT_FAILED = 4

# Seconds to wait before each retry of a comment. LinkedIn can answer 404 for a
# post it created a second earlier, so a short wait fixes the common failure;
# a module constant so tests don't sleep.
COMMENT_RETRY_DELAYS = (5, 15)
_RETRYABLE_COMMENT_STATUS = {404, 429, 500, 502, 503, 504}

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


def _item(issue: dict, post_number: int) -> dict:
    items = issue.get("items", [])
    if post_number < 1 or post_number > len(items):
        raise IndexError(f"post_number must be between 1 and {len(items)}")
    return items[post_number - 1]


# Tells the reader where the link went, in the issue's language.
LINK_NOTES = {"en": "🔗 Full article in the first comment 👇",
              "es": "🔗 Artículo completo en el primer comentario 👇"}


def post_body(issue: dict, post_number: int) -> str:
    """Item N as a LinkedIn post: the text, a note pointing at the first
    comment, and the publication's own call to action — no link, which goes in
    the comment (see comment_text). Every part of it is looked up, not parsed."""
    item = _item(issue, post_number)
    emoji = item.get("emoji", "").strip()
    text = f"{emoji} {item['text']}".strip()
    note = LINK_NOTES.get(issue.get("language", "en"), LINK_NOTES["en"])
    cta = (issue.get("cta") or "").strip()
    return "\n\n".join(part for part in (text, note, cta) if part)


def comment_text(issue: dict, post_number: int) -> str:
    """The first comment on item N: its source URL, the one the harvester
    fetched and the publisher link-checked."""
    return _item(issue, post_number)["url"]


# ── What has been posted ──────────────────────────────────────────────────────

def posted_path(work_dir: Path, publication: str, target_date: date) -> Path:
    return work_dir / "posted" / publication / f"{target_date.isoformat()}.json"


def load_posted(work_dir: Path, publication: str, target_date: date) -> dict:
    """{slot: {post_id, posted_at, comment: "pending"|"done", comment_error?}}.

    A missing file is an empty record; an unreadable one is an error. Treating
    a corrupt record as empty would let the next cron slot repost the day."""
    path = posted_path(work_dir, publication, target_date)
    if not path.exists():
        return {}
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(record, dict):
            raise ValueError("not a JSON object")
        return record
    except (ValueError, OSError) as e:
        raise RuntimeError(f"cannot read the posted record {path}: {e} — fix or "
                           f"remove it; refusing to guess what was already posted.")


def save_posted(work_dir: Path, publication: str, target_date: date, record: dict):
    path = posted_path(work_dir, publication, target_date)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(record, indent=1, ensure_ascii=False), encoding="utf-8")
    os.replace(tmp, path)


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


def post_to_linkedin(text: str, token: str, image=None, title: str = "",
                     author: str = None) -> str:
    """Posts `text`, with `image` attached when there is one and it uploads.

    Only the *upload* falls back to text-only. A failure creating the post
    itself is raised and never retried: it may have half gone through, and
    retrying is how a profile ends up with the same item twice."""
    author = author or get_person_urn(token)

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


def post_comment(post_urn: str, text: str, token: str, author: str) -> None:
    """Adds `text` as a comment on `post_urn`, as `author`. Raises RuntimeError
    when it cannot.

    Unlike the post, the comment is retried — but only on failures that mean
    "not yet" (404 right after creation, 429, 5xx, a dropped connection). A 403
    is a missing permission, and asking again will not grant it."""
    url = ("https://api.linkedin.com/v2/socialActions/"
           f"{quote(post_urn, safe='')}/comments")
    headers = {"Authorization": f"Bearer {token}",
               "Content-Type": "application/json",
               "X-Restli-Protocol-Version": "2.0.0"}
    body = {"actor": author, "message": {"text": text}}

    last = "no attempt made"
    for delay in (0,) + tuple(COMMENT_RETRY_DELAYS):
        if delay:
            time.sleep(delay)
        try:
            r = requests.post(url, headers=headers, json=body)
        except Exception as e:                        # noqa: BLE001 — retried below
            last = f"{type(e).__name__}: {e}"
            continue
        if r.status_code < 400:
            return
        last = f"HTTP {r.status_code} {(r.text or '')[:200]}".strip()
        if r.status_code not in _RETRYABLE_COMMENT_STATUS:
            break
    raise RuntimeError(f"comment on {post_urn} failed: {last}")


def finish_comments(issue: dict, posted: dict, work_dir: Path, publication: str,
                    target_date: date, token: str, author: str) -> list:
    """Adds every comment still owed today, lowest slot first, saving after each
    so a crash half way loses nothing. Returns the slots that still owe one."""
    still_owed = []
    for slot in sorted((k for k, v in posted.items() if v.get("comment") != "done"),
                       key=int):
        entry = posted[slot]
        try:
            post_comment(entry["post_id"], comment_text(issue, int(slot)), token, author)
        except (RuntimeError, IndexError, KeyError) as e:
            print(f"WARNING: slot {slot} is posted but has no link comment yet: {e}")
            entry.update(comment="pending", comment_error=str(e))
            still_owed.append(int(slot))
        else:
            print(f"Commented the source link on slot {slot}.")
            entry.update(comment="done")
            entry.pop("comment_error", None)
        save_posted(work_dir, publication, target_date, posted)
    return still_owed


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
    print(f"first comment: {comment_text(issue, post_number)}")
    print(f"image: {image}" if image else "no image for this slot (text-only)")
    if dry_run:
        print("[DRY RUN] nothing was posted.")
        return 0

    try:
        posted = load_posted(work_dir, publication, target_date)
    except RuntimeError as e:
        print(f"ERROR: {e}")
        return 1

    slot = str(post_number)
    already = bool(posted.get(slot, {}).get("post_id"))
    owes_comment = any(v.get("comment") != "done" for v in posted.values())
    if already and not owes_comment:
        print(f"Slot {post_number} is already posted and commented — nothing to do.")
        return 0

    token = get_valid_token(load_env(work_dir / ".env"))
    author = get_person_urn(token)

    if already:
        print(f"Slot {post_number} is already posted ({posted[slot]['post_id']}); "
              "not posting again.")
    else:
        post_id = post_to_linkedin(text, token, image, title, author=author)
        # Written before the comment is attempted: from here on the post is
        # public, and this record is what stops a re-run from making a second.
        posted[slot] = {"post_id": post_id, "comment": "pending",
                        "posted_at": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
        save_posted(work_dir, publication, target_date, posted)
        print(f"Posted successfully! Post ID: {post_id}")

    if finish_comments(issue, posted, work_dir, publication, target_date,
                       token, author):
        print("Exit 4: posted, but a link comment is still owed.")
        return EXIT_COMMENT_FAILED
    return 0


if __name__ == "__main__":
    sys.exit(main())
