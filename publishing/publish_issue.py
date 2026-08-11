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

# ── Deterministic link repair ────────────────────────────────────────────────
# A broken link at send time (2026-08-11: a washingtonpost.com timeout) used to
# come back to curator as a tool rejection, asking a language model to notice
# and swap the item — which it wasn't reliably doing, and which is exactly the
# kind of mechanical step the deterministic-over-agentic principle says should
# be code instead. Repairing it here means the model is never involved: a
# broken item is either swapped for an already-harvested candidate covering
# the same story, or dropped, before the model ever sees the outcome.

# Mirrors src/core/tools/issue.cpp's content_words/split_sentences/
# kMinOverlap — the same "does this post actually match this page" gate
# build_issue used at selection time, applied again to whatever page a
# repair is about to substitute in. Not required to match byte-for-byte
# (this is a second, independent safety gate on an automatic repair, not
# the primary check), but keep the thresholds in sync if either changes.
_STOP_WORDS = {
    "the", "a", "an", "and", "or", "but", "in", "on", "at", "to", "for",
    "of", "is", "are", "was", "were", "be", "been", "has", "have", "had",
    "will", "can", "its", "this", "that", "with", "from", "by", "not",
    "also", "than", "more", "most", "into", "over", "just", "about",
    "after", "before", "between", "through", "under", "such", "very",
    "only", "even", "both", "each", "all", "any", "some", "new", "first",
    "last", "other", "same", "which", "their", "them", "they", "what",
    "when", "where", "how", "who", "would", "could", "should", "does",
    "did", "being", "here", "there", "then", "now", "out", "own", "these",
    "those", "said", "says", "one", "two", "may", "like", "well", "use",
    "used", "using", "make", "made", "get", "got", "set", "way", "many",
}
_MIN_WORD_LEN = 4
_MIN_SENTENCE_CHARS = 40
_MIN_OVERLAP = 3


def _content_words(text):
    words = set()
    word = ""
    for ch in text:
        if ch.isalnum():
            word += ch.lower()
            continue
        if len(word) >= _MIN_WORD_LEN and word not in _STOP_WORDS:
            words.add(word)
        word = ""
    if len(word) >= _MIN_WORD_LEN and word not in _STOP_WORDS:
        words.add(word)
    return words


def _split_sentences(text):
    # Every newline is a structural break in extracted web text (it marks a
    # different HTML element) — without this, nav items concatenate into one
    # blob that can score well because real article text is buried in it.
    sentences = []
    current = ""
    for i, ch in enumerate(text):
        current += ch
        is_break = ch == "\n"
        if ch in ".!?" and (i + 1 >= len(text) or text[i + 1].isspace()):
            is_break = True
        if is_break:
            s = current.strip()
            if len(s) >= _MIN_SENTENCE_CHARS:
                sentences.append(s)
            current = ""
    s = current.strip()
    if len(s) >= _MIN_SENTENCE_CHARS:
        sentences.append(s)
    return sentences


def best_overlap(post_text, page_text):
    """The most content words any single sentence of page_text shares with
    post_text. 0 if page_text is empty or post_text has no content words."""
    post_words = _content_words(post_text)
    if not post_words or not page_text:
        return 0
    best = 0
    for sentence in _split_sentences(page_text):
        overlap = len(post_words & _content_words(sentence))
        if overlap > best:
            best = overlap
    return best


def load_pool(work_dir, publication, target_date):
    """The full harvested candidate pool (every candidate fetched, not just
    the ones selected) — the same file src/core/tools/harvest.cpp writes and
    src/core/tools/issue.cpp's publish_issue reads. None if it's missing or
    unreadable, which a repair pass treats as "no alternates available"
    rather than an error: the issue itself is still valid without it."""
    path = work_dir / f"harvest_{publication}_{target_date.isoformat()}.json"
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None


def _headline(title, max_chars=60):
    """Same intent as issue.cpp's headline_from_title, simplified: Python
    strings are already character-addressable, so no manual UTF-8 safety is
    needed. Only used for a repaired item's headline — the original item's
    headline already went through the real one in build_issue."""
    clean = title.strip()
    for sep in (" | ", " - ", " – ", " — "):
        at = clean.find(sep)
        if at >= 20:
            clean = clean[:at]
            break
    if len(clean) <= max_chars:
        return clean
    cut = clean[:max_chars]
    space = cut.rfind(" ")
    if space > max_chars // 2:
        cut = cut[:space]
    return cut.strip() + "…"


def find_replacement(item, candidates_by_id, used_ids):
    """A different, not-yet-used pool candidate covering the same story as
    item's original candidate, whose link resolves and whose page still
    supports the post's own text. Returns (candidate, status, detail) or
    None if no such candidate exists."""
    original = candidates_by_id.get(item.get("candidate_id"))
    if original is None or original.get("story") is None:
        return None
    story = original["story"]
    for cid, candidate in candidates_by_id.items():
        if cid in used_ids or cid == item.get("candidate_id"):
            continue
        if candidate.get("story") != story:
            continue
        if best_overlap(item["text"], candidate.get("text", "")) < _MIN_OVERLAP:
            continue
        status, detail = check_link(candidate.get("url", ""))
        if status == "broken":
            continue
        return candidate, status, detail
    return None


def repair_and_check(items, pool):
    """Re-checks every item's link. A broken one is repaired in place from a
    same-story pool alternate if one resolves and still supports the post's
    text; otherwise the item is dropped rather than blocking the whole issue
    over one story. Items are renumbered afterward so post_tweet.py's fixed
    n-indexes stay contiguous.

    Returns (kept_items, results, repaired, dropped) — results is
    [(item, status, detail)] for every surviving item, matching what
    check_all used to return; repaired/dropped are for the run record.
    """
    used_ids = {item.get("candidate_id") for item in items}
    candidates_by_id = {c.get("id"): c for c in pool.get("candidates", [])} if pool else {}

    kept, results, repaired, dropped = [], [], [], []
    print("Checking links:")
    for item in items:
        status, detail = check_link(item["url"])
        print(f"  {status.upper():8} {detail:24} #{item['n']} {item['url']}", flush=True)

        if status != "broken":
            results.append((item, status, detail))
            kept.append(item)
            continue

        found = find_replacement(item, candidates_by_id, used_ids)
        if found is None:
            print(f"  DROPPED  #{item['n']} — no usable alternate for this story",
                  flush=True)
            dropped.append({"n": item["n"], "url": item["url"], "reason": detail})
            continue

        candidate, new_status, new_detail = found
        old_url = item["url"]
        item = dict(item)
        item["url"] = candidate.get("url", "")
        item["source"] = candidate.get("source", item.get("source", ""))
        item["published"] = candidate.get("published", item.get("published", ""))
        item["headline"] = _headline(candidate.get("title", ""))
        item["candidate_id"] = candidate.get("id")
        used_ids.add(candidate.get("id"))
        print(f"  REPAIRED #{item['n']} {old_url} -> {item['url']} "
              f"(same story, candidate {candidate.get('id')})", flush=True)
        repaired.append({"n": item["n"], "old_url": old_url, "new_url": item["url"]})
        kept.append(item)
        results.append((item, new_status, new_detail))

    for n, item in enumerate(kept, 1):
        item["n"] = n

    return kept, results, repaired, dropped


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

    original_items = issue["items"]
    record["harvested"] = issue.get("harvested", 0)
    print(f"Issue {issue.get('title', publication)} {target_date.isoformat()}: "
          f"{len(original_items)} item(s) selected from {record['harvested']} harvested")

    # Link-checking (and any repair it triggers) happens before rendering, not
    # after: a repaired item's new URL/headline has to reach the artifacts,
    # not just the run record.
    pool = load_pool(work_dir, publication, target_date)
    items, results, repaired, dropped = repair_and_check(original_items, pool)
    issue["items"] = items
    record["selected"] = len(items)
    if repaired:
        record["repaired"] = repaired
    if dropped:
        record["dropped"] = dropped

    suspect = [(i, d) for i, s, d in results if s == "suspect"]
    record["links"] = {"ok": len(results) - len(suspect), "suspect": len(suspect),
                       "broken": len(dropped)}

    if suspect:
        # Bot-blocked, not dead. Recorded, never blocking: news sites return
        # these to non-browser clients often enough that blocking on them would
        # stall every issue.
        print(f"\n{len(suspect)} SUSPECT link(s) — likely bot-blocking:")
        for item, detail in suspect:
            print(f"  #{item['n']} {detail} {item['url']}")

    min_items = issue.get("min_items", 1)
    if len(items) < min_items:
        why = (f"{len(dropped)} broken link(s) had no usable replacement; "
                f"{len(items)} item(s) remain, this publication needs at least "
                f"{min_items}")
        print(f"\n{why}. Nothing was sent.")
        record["reason"] = why
        record["duration_s"] = round(time.time() - started, 1)
        write_run_record(work_dir, publication, record)
        return 2

    print(f"\nAll {len(items)} link(s) resolve" + (" (after repair)." if repaired else "."))

    try:
        written = render_artifacts(issue, work_dir, target_date)
    except (ValueError, FileNotFoundError, OSError) as e:
        print(f"ERROR: {e}")
        record["reason"] = str(e)
        write_run_record(work_dir, publication, record)
        return 1
    record["artifacts"] = [kind for kind, _ in written]

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
