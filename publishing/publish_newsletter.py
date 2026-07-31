#!/usr/bin/env python3
"""
publish_newsletter.py — renders and (optionally) sends the day's AI Pulse issue.

Everything mechanical about publishing lives here rather than in an LLM agent's
step list: reading X_posts_<date>.txt, rendering newsletter_<date>.html from
newsletter_template.html, and checking that every link resolves. Those steps are
pure string and IO work with one right answer, and asking a model to walk them
one tool call at a time just adds places for it to stop early or hallucinate
success. The agent's job shrinks to the two things that need judgment: writing
the posts file, and replacing a source URL that turns out to be dead.

Usage:
    python3 publish_newsletter.py [YYYY-MM-DD]                     # render + check
    python3 publish_newsletter.py [YYYY-MM-DD] --send              # ...then send
    python3 publish_newsletter.py [YYYY-MM-DD] --send --dry-run    # ...pretend to
    python3 publish_newsletter.py --self-test                      # unit tests

    Date defaults to today.

The issue directory — where X_posts_<date>.txt is read and newsletter_<date>.html
is written — is `--dir PATH`, else $FUNES_PUBLISH_DIR, else the current working
directory. It is deliberately not the script's own directory: this file lives in
the Funes repo and is deployed by `git pull`, while the issues, the subscriber
list and .env live outside it on the host that sends them.

Exit codes (a caller — human or agent — can branch on these):
    0  success. With --send, the newsletter was actually sent.
    1  usage/input error: bad date, missing posts file, missing template.
    2  broken links found. Nothing was sent. Fix X_posts_<date>.txt and re-run.

--send refuses to send when links are broken, so "it went out with a dead link"
is not a reachable state. Links that merely look bot-blocked (401/403/429) are
reported as SUSPECT and do not block: news sites return those to non-browser
clients often enough that blocking on them would stall every issue.
"""

import html
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import date
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.resolve()
TEMPLATE_NAME = "newsletter_template.html"

# Sites reject obvious bots; this is a real browser's UA so a 403 means
# something closer to "actually gone" than "you're not Chrome".
USER_AGENT = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/126.0 Safari/537.36")
LINK_TIMEOUT = 12
# Long enough for a rate limiter or a slow origin to recover, short enough that
# ten of them don't dominate the run.
RETRY_PAUSE = 2

# Anti-bot / rate-limit responses: reported, never blocking.
SUSPECT_CODES = {401, 403, 429}

POST_START = re.compile(r"^\s*(\d+)\.\s*(.*)$")
# A bare URL, optionally followed by " — Headline" (em dash, en dash or pipe).
# The headline is the one part of the link label a script can't derive, so the
# agent may supply it; without one the label falls back to the bare host.
URL_LINE = re.compile(r"^\s*(https?://\S+)(?:\s*[—–|]\s*(.+?))?\s*$")
SEPARATOR = re.compile(r"^\s*-{3,}\s*$")


# ── Locations ─────────────────────────────────────────────────────────────────

def issue_dir(explicit=None):
    """Where the issue files live. --dir, else $FUNES_PUBLISH_DIR, else cwd."""
    return Path(explicit or os.environ.get("FUNES_PUBLISH_DIR") or Path.cwd()).resolve()


def template_path(work_dir, name=TEMPLATE_NAME):
    """The issue dir's own template wins, so a publication can override the
    repo's default without a fork; the repo copy is the fallback."""
    local = work_dir / name
    return local if local.exists() else SCRIPT_DIR / name


# ── Parsing ───────────────────────────────────────────────────────────────────

def parse_posts(text):
    """Parses X_posts_<date>.txt into [{"n", "text", "url"}].

    Forgiving on purpose — this file is written by an LLM, so it tolerates a
    missing separator, post text wrapped over several lines, and a trailing
    header block. It does NOT tolerate a post with no URL: a newsletter item
    without a source is the exact defect the link check exists to catch, and
    silently dropping it would hide it.
    """
    posts = []
    current = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if SEPARATOR.match(line):
            continue
        m = POST_START.match(line)
        if m:
            if current:
                posts.append(current)
            current = {"n": int(m.group(1)), "text": m.group(2).strip(),
                       "url": None, "headline": None}
            continue
        if current is None:
            continue                      # header lines before post 1
        u = URL_LINE.match(line)
        if u:
            current["url"] = u.group(1)
            current["headline"] = (u.group(2) or "").strip() or None
            continue
        if line.strip():                  # continuation of the post text
            current["text"] = (current["text"] + " " + line.strip()).strip()
    if current:
        posts.append(current)

    missing = [p["n"] for p in posts if not p["url"]]
    if missing:
        raise ValueError(f"post(s) {missing} have no source URL")
    if not posts:
        raise ValueError("no posts found — expected lines like '1. 🚀 text' "
                         "followed by a bare URL")
    return posts


# ── Rendering ─────────────────────────────────────────────────────────────────

def link_label(post):
    """"host — Headline", or just the host when no headline was supplied."""
    host = re.sub(r"^https?://", "", post["url"]).split("/")[0]
    host = re.sub(r"^www\.", "", host)
    headline = post.get("headline")
    return f"{host} — {headline}" if headline else host


def render_post_item(post):
    # quote=False: apostrophes and quotes need no escaping in element text, and
    # leaving them alone keeps the output readable when diffing two issues.
    # The href is escaped with quote=True, where it does matter.
    return (
        '<div class="post-item">\n'
        f'  <div class="post-number">{post["n"]}</div>\n'
        '  <div class="post-body">\n'
        f'    <div class="post-text">{html.escape(post["text"], quote=False)}</div>\n'
        f'    <a class="post-link" href="{html.escape(post["url"], quote=True)}">'
        f'{html.escape(link_label(post), quote=False)}</a>\n'
        '  </div>\n'
        '</div>'
    )


def placeholder_indent(template, placeholder):
    """The leading whitespace on the template line holding `placeholder`.

    The rendered blocks are indented to match, so the output lines up with the
    surrounding markup instead of stair-stepping off the first item (whose
    indentation comes from the template, not from us)."""
    for line in template.splitlines():
        if placeholder in line:
            return line[:len(line) - len(line.lstrip())]
    return ""


def render_html(template, target_date, posts):
    # "Wednesday, July 30, 2026" — matches the existing issues' convention.
    friendly = target_date.strftime("%A, %B %d, %Y").replace(" 0", " ")
    indent = placeholder_indent(template, "{{POSTS}}")
    blocks = []
    for i, post in enumerate(posts):
        lines = render_post_item(post).split("\n")
        # First line inherits the placeholder's own indentation from the
        # template; every later line has to be indented explicitly.
        body = "\n".join((indent + l if (i or n) else l)
                         for n, l in enumerate(lines))
        blocks.append(body)
    return (template.replace("{{DATE}}", friendly)
                    .replace("{{POSTS}}", "\n\n".join(blocks)))


# ── Link checking ─────────────────────────────────────────────────────────────

def _attempt(url):
    """One HEAD-then-GET pass. Returns (status, detail, transport_failure)."""
    for method in ("HEAD", "GET"):
        req = urllib.request.Request(url, method=method,
                                     headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=LINK_TIMEOUT) as resp:
                return "ok", str(resp.status), False
        except urllib.error.HTTPError as e:
            # Plenty of servers refuse HEAD but serve GET fine.
            if e.code in (403, 405, 501) and method == "HEAD":
                continue
            if e.code in SUSPECT_CODES:
                return "suspect", f"HTTP {e.code}", False
            # The server answered. Whatever it said, it said it on purpose.
            return "broken", f"HTTP {e.code}", False
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            if method == "HEAD":
                continue
            return "broken", f"{type(e).__name__}: {e}", True
        except ValueError as e:                     # malformed URL
            return "broken", f"invalid URL: {e}", False
    return "broken", "no response to HEAD or GET", True


def check_link(url, retries=1):
    """Returns (status, detail). status is one of ok / suspect / broken.

    A transport failure — a timeout, a refused connection, a DNS blip — is
    retried, because it is a statement about the network at this instant rather
    than about the URL. On 2026-07-31 washingtonpost.com was fetched fine by the
    harvester and timed out on this check a minute later, which was enough to
    refuse the whole issue. An HTTP status is never retried: the server answered,
    and asking again will not change its mind.
    """
    for attempt in range(retries + 1):
        status, detail, transport_failure = _attempt(url)
        if not transport_failure or attempt == retries:
            return status, detail
        time.sleep(RETRY_PAUSE)
    return "broken", "no response to HEAD or GET"     # unreachable; keeps the contract


def check_all(posts):
    results = []
    for p in posts:
        status, detail = check_link(p["url"])
        results.append((p, status, detail))
        print(f"  {status.upper():8} {detail:24} #{p['n']} {p['url']}", flush=True)
    return results


# ── Main ──────────────────────────────────────────────────────────────────────

def publish(target_date, send, dry_run=False, work_dir=None):
    work_dir = work_dir or issue_dir()
    posts_path = work_dir / f"X_posts_{target_date.isoformat()}.txt"
    html_path = work_dir / f"newsletter_{target_date.isoformat()}.html"
    template = template_path(work_dir)

    if not posts_path.exists():
        print(f"ERROR: posts file not found: {posts_path}")
        return 1
    if not template.exists():
        print(f"ERROR: template not found: {template}")
        return 1

    try:
        posts = parse_posts(posts_path.read_text(encoding="utf-8"))
    except ValueError as e:
        print(f"ERROR: could not parse {posts_path.name}: {e}")
        return 1
    print(f"Parsed {len(posts)} post(s) from {posts_path.name}")

    html_path.write_text(
        render_html(template.read_text(encoding="utf-8"), target_date, posts),
        encoding="utf-8")
    print(f"Rendered {html_path.name} ({html_path.stat().st_size} bytes)")

    print("Checking links:")
    results = check_all(posts)
    broken = [(p, d) for p, s, d in results if s == "broken"]
    suspect = [(p, d) for p, s, d in results if s == "suspect"]

    if suspect:
        print(f"\n{len(suspect)} SUSPECT link(s) — likely bot-blocking, not "
              f"blocking the send. Worth a manual look:")
        for p, d in suspect:
            print(f"  #{p['n']} {d} {p['url']}")

    if broken:
        print(f"\nBROKEN: {len(broken)} link(s) do not resolve. Nothing was sent.")
        for p, d in broken:
            print(f"  #{p['n']} {d} {p['url']}")
        print(f"\nFix these in {posts_path.name} — find the real primary source "
              f"for each, correct the post text if the correction changes the "
              f"facts, then run this script again.")
        return 2

    print(f"\nAll {len(posts)} link(s) resolve.")

    if not send:
        print("Render + check only (no --send). Nothing was sent.")
        return 0

    print("\nSending:" if not dry_run else "\nSending (DRY RUN — no mail leaves):")
    cmd = [sys.executable, str(SCRIPT_DIR / "send_newsletter.py"),
           target_date.isoformat(), "--dir", str(work_dir)]
    if dry_run:
        cmd.append("--dry-run")
    proc = subprocess.run(cmd, cwd=work_dir, capture_output=True, text=True)
    print(proc.stdout.strip())
    if proc.stderr.strip():
        print(proc.stderr.strip())
    if proc.returncode != 0:
        print(f"\nSEND FAILED: send_newsletter.py exited {proc.returncode}. "
              f"The newsletter was NOT sent.")
        return proc.returncode
    if dry_run:
        print(f"\nDRY RUN OK: newsletter_{target_date.isoformat()}.html would "
              f"have been sent. No mail was actually delivered.")
        return 0
    print(f"\nSENT: newsletter_{target_date.isoformat()}.html went out to all "
          f"subscribers (send_newsletter.py exit 0).")
    return 0


def self_test():
    """Runs the unit suite in publishing/tests/ — the same tests CI runs, so
    there is one set of assertions rather than a deployed copy that drifts.
    Kept as a flag because on the sending host this script is the only entry
    point anyone remembers."""
    import unittest
    tests_dir = SCRIPT_DIR / "tests"
    if not tests_dir.is_dir():
        print(f"ERROR: no test directory at {tests_dir} — is this a full checkout?")
        return 1
    suite = unittest.defaultTestLoader.discover(str(tests_dir), top_level_dir=str(SCRIPT_DIR))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


def main():
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()

    send = "--send" in args
    explicit_dir = None
    if "--dir" in args:
        i = args.index("--dir")
        if i + 1 >= len(args):
            print("ERROR: --dir needs a path")
            return 1
        explicit_dir = args[i + 1]
        args = args[:i] + args[i + 2:]

    positional = [a for a in args if not a.startswith("-")]
    if positional:
        try:
            target_date = date.fromisoformat(positional[0])
        except ValueError:
            print(f"ERROR: invalid date '{positional[0]}' — use YYYY-MM-DD")
            return 1
    else:
        target_date = date.today()

    return publish(target_date, send, dry_run="--dry-run" in args,
                   work_dir=issue_dir(explicit_dir))


if __name__ == "__main__":
    sys.exit(main())
