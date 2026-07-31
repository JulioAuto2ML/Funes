#!/usr/bin/env python3
"""
publish_issue.py — renders, checks and sends an issue from its JSON record.

The difference from publish_newsletter.py is what it trusts. That script reads
X_posts_<date>.txt, a file written by hand by a language model, through a
forgiving regex parser — and a second, different parser in post_tweet.py reads
the same file for LinkedIn. Two hand-rolled parsers of an undocumented format
that a model types out is three fragile things stacked, and the URLs in it are
retyped from a transcript, which is how an item about an OpenAI breach came to
link to a Stripe checkout page.

Here the issue JSON is the source of truth. It is written by the publish_issue
tool, whose URLs come from the pages the harvester actually fetched, and whose
evidence quotes have already been checked against those pages. The .txt and the
HTML become artifacts generated from it, which is also what finally gives the
LinkedIn cron a link nobody retyped.

Usage:
    python3 publish_issue.py [YYYY-MM-DD] [--publication ID] [--dir PATH]
                             [--send] [--dry-run]

    Date defaults to today; publication defaults to ai-pulse.

Reads   <dir>/issues/<publication>/<date>.json
Writes  <dir>/X_posts_<date>.txt          (the posts, for LinkedIn)
        <dir>/newsletter_<date>.html      (the newsletter)
        <dir>/runs/<publication>/<date>.json   (what happened)

Exit codes, the same contract publish_newsletter.py has:
    0  success. With --send, the newsletter was actually sent.
    1  usage/input error: bad date, missing issue, missing template.
    2  broken links found. Nothing was sent.

A run record is written on every outcome, including the blocked ones: a
scheduled run that fails silently is indistinguishable from one that never
started, and the record is what a human finds the next morning.
"""

import json
import subprocess
import sys
import time
from datetime import date
from pathlib import Path

from publish_newsletter import (
    SCRIPT_DIR,
    check_link,
    issue_dir,
    render_html,
    template_path,
)


def load_issue(work_dir, publication, target_date):
    path = work_dir / "issues" / publication / f"{target_date.isoformat()}.json"
    if not path.exists():
        raise FileNotFoundError(f"no issue at {path}")
    issue = json.loads(path.read_text(encoding="utf-8"))
    items = issue.get("items")
    if not items:
        raise ValueError(f"{path.name} has no items")
    for i, item in enumerate(items, 1):
        for field in ("text", "url"):
            if not item.get(field):
                raise ValueError(f"{path.name} item {i} has no {field}")
    return issue


def render_posts_txt(issue, target_date):
    """The posts file, still in the shape post_tweet.py and a human expect —
    but generated, so nobody types a URL into it."""
    friendly = target_date.strftime("%B %d, %Y").replace(" 0", " ")
    out = [f"X Posts — {friendly}",
           "Generated from trending AI/LLM/AGI topics",
           "=" * 60, ""]
    for item in issue["items"]:
        emoji = item.get("emoji", "").strip()
        headline = item.get("headline", "").strip()
        out.append(f"{item['n']}. {emoji + ' ' if emoji else ''}{item['text']}".rstrip())
        out.append(item["url"] + (f" — {headline}" if headline else ""))
        out += ["", "---", ""]
    return "\n".join(out[:-3]) + "\n"


def posts_for_render(issue):
    """The issue's items in the shape render_html expects. The link label comes
    from the harvested title, so it is the article's own headline rather than
    one more thing to get wrong."""
    return [{"n": item["n"],
             "text": f"{item.get('emoji', '')} {item['text']}".strip(),
             "url": item["url"],
             "headline": item.get("headline") or None}
            for item in issue["items"]]


DEFAULT_ARTIFACTS = [
    {"kind": "posts_txt", "path": "{workspace}/X_posts_{iso_date}.txt"},
    {"kind": "newsletter_html", "path": "{workspace}/newsletter_{iso_date}.html",
     "template": "newsletter_template.html"},
]


def substitute(template, work_dir, target_date):
    """The two placeholders a publication config may use in a path or subject."""
    return (template.replace("{workspace}", str(work_dir))
                    .replace("{iso_date}", target_date.isoformat())
                    .replace("{date}",
                             target_date.strftime("%A, %B %d, %Y").replace(" 0", " ")))


def artifacts_of(issue):
    """A publication with no configured artifacts gets the two the AI newsletter
    has always produced — the config is meant to describe an existing thing
    before it is meant to change it."""
    return issue.get("artifacts") or DEFAULT_ARTIFACTS


def render_artifacts(issue, work_dir, target_date):
    """Returns [(kind, path)] for what was written. An unknown kind is an error:
    a config asking for something this script can't produce should say so rather
    than quietly ship an issue missing half of it."""
    written = []
    for artifact in artifacts_of(issue):
        kind = artifact.get("kind")
        path = Path(substitute(artifact.get("path", ""), work_dir, target_date))
        if not artifact.get("path"):
            raise ValueError(f"artifact '{kind}' has no path")
        path.parent.mkdir(parents=True, exist_ok=True)

        if kind == "posts_txt":
            path.write_text(render_posts_txt(issue, target_date), encoding="utf-8")
        elif kind == "newsletter_html":
            template = template_path(work_dir,
                                     artifact.get("template") or "newsletter_template.html")
            if not template.exists():
                raise FileNotFoundError(f"template not found: {template}")
            path.write_text(
                render_html(template.read_text(encoding="utf-8"), target_date,
                            posts_for_render(issue)),
                encoding="utf-8")
        else:
            raise ValueError(f"unknown artifact kind '{kind}'")

        written.append((kind, path))
        print(f"Wrote {path.name} ({path.stat().st_size} bytes)")
    return written


def email_channel(issue):
    """The first email channel, or the default one. None means this publication
    has channels configured and email is not among them — nothing to send, which
    is a legitimate configuration and not a failure."""
    channels = issue.get("channels")
    if channels is None:
        return {"kind": "email", "recipients_file": "subscribers.txt"}
    for channel in channels:
        if channel.get("kind") == "email":
            return channel
    return None


def check_all(items):
    """Re-checks every link. The harvest may be an hour old, and this is the
    last gate before the mail leaves."""
    results = []
    print("Checking links:")
    for item in items:
        status, detail = check_link(item["url"])
        results.append((item, status, detail))
        print(f"  {status.upper():8} {detail:24} #{item['n']} {item['url']}", flush=True)
    return results


def write_run_record(work_dir, publication, record):
    path = work_dir / "runs" / publication / f"{record['date']}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(record, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"Run record: {path}")


def publish(target_date, publication, work_dir, send, dry_run=False):
    started = time.time()
    record = {"publication": publication, "date": target_date.isoformat(),
              "status": "blocked"}

    try:
        issue = load_issue(work_dir, publication, target_date)
    except (FileNotFoundError, ValueError, json.JSONDecodeError) as e:
        print(f"ERROR: {e}")
        record["reason"] = str(e)
        write_run_record(work_dir, publication, record)
        return 1

    items = issue["items"]
    record["harvested"] = issue.get("harvested", 0)
    record["selected"] = len(items)
    print(f"Issue {issue.get('title', publication)} {target_date.isoformat()}: "
          f"{len(items)} item(s) selected from {record['harvested']} harvested")

    try:
        written = render_artifacts(issue, work_dir, target_date)
    except (ValueError, FileNotFoundError, OSError) as e:
        print(f"ERROR: {e}")
        record["reason"] = str(e)
        write_run_record(work_dir, publication, record)
        return 1
    record["artifacts"] = [kind for kind, _ in written]

    results = check_all(items)
    broken = [(i, d) for i, s, d in results if s == "broken"]
    suspect = [(i, d) for i, s, d in results if s == "suspect"]
    record["links"] = {"ok": len(results) - len(broken) - len(suspect),
                       "suspect": len(suspect), "broken": len(broken)}

    if suspect:
        # Bot-blocked, not dead. Recorded, never blocking: news sites return
        # these to non-browser clients often enough that blocking on them would
        # stall every issue.
        print(f"\n{len(suspect)} SUSPECT link(s) — likely bot-blocking:")
        for item, detail in suspect:
            print(f"  #{item['n']} {detail} {item['url']}")

    if broken:
        print(f"\nBROKEN: {len(broken)} link(s) do not resolve. Nothing was sent.")
        for item, detail in broken:
            print(f"  #{item['n']} {detail} {item['url']}")
        record["reason"] = f"{len(broken)} broken link(s)"
        record["rejected"] = [{"n": i["n"], "url": i["url"], "reason": d}
                              for i, d in broken]
        record["duration_s"] = round(time.time() - started, 1)
        write_run_record(work_dir, publication, record)
        return 2

    print(f"\nAll {len(items)} link(s) resolve.")

    if not send:
        print("Render + check only (no --send). Nothing was sent.")
        record["status"] = "rendered"
        record["duration_s"] = round(time.time() - started, 1)
        write_run_record(work_dir, publication, record)
        return 0

    channel = email_channel(issue)
    if channel is None:
        print("\nNo email channel configured for this publication — nothing to send.")
        record["status"] = "rendered"
        record["reason"] = "no email channel"
        record["duration_s"] = round(time.time() - started, 1)
        write_run_record(work_dir, publication, record)
        return 0

    print("\nSending:" if not dry_run else "\nSending (DRY RUN — no mail leaves):")
    cmd = [sys.executable, str(SCRIPT_DIR / "send_newsletter.py"),
           target_date.isoformat(), "--dir", str(work_dir)]
    if channel.get("recipients_file"):
        cmd += ["--recipients", channel["recipients_file"]]
    if issue.get("subject"):
        cmd += ["--subject", substitute(issue["subject"], work_dir, target_date)]
    if dry_run:
        cmd.append("--dry-run")
    proc = subprocess.run(cmd, cwd=work_dir, capture_output=True, text=True)
    print(proc.stdout.strip())
    if proc.stderr.strip():
        print(proc.stderr.strip())

    record["duration_s"] = round(time.time() - started, 1)
    if proc.returncode != 0:
        print(f"\nSEND FAILED: send_newsletter.py exited {proc.returncode}. "
              f"The newsletter was NOT sent.")
        record["reason"] = f"send_newsletter.py exited {proc.returncode}"
        write_run_record(work_dir, publication, record)
        return proc.returncode

    record["status"] = "dry-run" if dry_run else "sent"
    record["recipients"] = proc.stdout.count("✓ Sent to")
    write_run_record(work_dir, publication, record)
    print(f"\n{'DRY RUN OK' if dry_run else 'SENT'}: "
          f"newsletter_{target_date.isoformat()}.html "
          f"{'would have gone' if dry_run else 'went'} out.")
    return 0


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

    positional = [a for a in args if not a.startswith("-")]
    if positional:
        try:
            target_date = date.fromisoformat(positional[0])
        except ValueError:
            print(f"ERROR: invalid date '{positional[0]}' — use YYYY-MM-DD")
            return 1
    else:
        target_date = date.today()

    return publish(target_date, publication, issue_dir(explicit_dir),
                   send="--send" in args, dry_run="--dry-run" in args)


if __name__ == "__main__":
    sys.exit(main())
