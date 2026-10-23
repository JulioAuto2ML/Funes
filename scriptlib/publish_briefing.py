#!/usr/bin/env python3
"""publish_briefing — publish the spoken news briefing as an Alexa news feed.

Run by Funes through `run_script` (see publish_briefing.yaml); stdout is exactly
one JSON object, everything else goes to stderr. Standard library only.

    --text=<the briefing, plain spoken Spanish> [--title=<shown in the Alexa app>]

Writes FUNES_NEWS_FEED_DIR/feed.json in the Flash Briefing feed format — one
text item: uid, updateDate, titleText, mainText, redirectionUrl — which
`tailscale funnel` serves at FUNES_NEWS_PUBLIC_URL and the Echo reads aloud
when asked for the news. Alexa polls the feed every 15 minutes.

What this script decides for itself rather than trusting the model:

- **Whether the text can be read aloud.** Alexa reads `mainText` exactly as
  written. A URL is spelled out letter by letter, markdown is read as
  punctuation or dropped, a tag is refused by Alexa, a briefing that stops
  mid-sentence sounds broken. Each of those is refused here with the reason, so
  the model can fix the text and publish again.
- **How long it is.** MIN_CHARS to MAX_CHARS. Alexa cuts text over 4,500
  characters at a sentence; this refuses rather than letting the end vanish.
- **What the feed holds.** Exactly one item, replaced on every publish:
  yesterday's briefing must not play before today's.
- **That the feed is whole.** Written to a temporary file in the same
  directory and renamed into place; a failed write leaves the previous feed
  as it was. The directory must already exist and be absolute — it is created
  by whoever sets up the funnel, never by a model-driven run.

Exit codes: 0 published, 1 the write failed, 2 the text was refused,
3 not configured (an installation fault, not something a retry fixes).

Config, from the manifest's env (config/funes.local):
    FUNES_NEWS_FEED_DIR    absolute directory the funnel serves (outside every
                           workspace, so no agent file tool can edit the feed)
    FUNES_NEWS_PUBLIC_URL  the public https URL of feed.json
"""

import argparse
import json
import os
import re
import sys
import tempfile
import uuid
from datetime import datetime, timezone
from pathlib import Path

MIN_CHARS = 300
MAX_CHARS = 4000
MAX_TITLE = 120
WORDS_PER_SECOND = 2.5  # ~150 words a minute
MONTHS = ["enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
          "agosto", "septiembre", "octubre", "noviembre", "diciembre"]

URL_RE = re.compile(r"https?://|www\.|\b[a-z0-9-]+\.(?:com|net|org|gob|gov|io|ar)\b", re.IGNORECASE)
MARKDOWN_RE = re.compile(r"\*\*|__|^\s*#+\s|^\s*[-*+•]\s|\[[^\]]*\]\(|`", re.MULTILINE)
TAG_RE = re.compile(r"<[^>]+>")
CONTROL_RE = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
SENTENCE_END = (".", "!", "?", "…", "».", ".»", '."', '".', ".)", ").")


class BriefingError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def _now():
    return datetime.now(timezone.utc)


def check_text(raw):
    text = CONTROL_RE.sub("", raw).strip()
    if len(text) < MIN_CHARS:
        raise BriefingError(2, f"publish_briefing: the text is {len(text)} characters; a briefing needs at least {MIN_CHARS}")
    if len(text) > MAX_CHARS:
        raise BriefingError(2, f"publish_briefing: the text is {len(text)} characters, the limit is {MAX_CHARS}; shorten it and publish again")
    if URL_RE.search(text):
        raise BriefingError(2, "publish_briefing: the text contains a URL or web address; Alexa would spell it out — name the outlet instead")
    if MARKDOWN_RE.search(text):
        raise BriefingError(2, "publish_briefing: the text contains markdown (**, #, list dashes, [links], backticks); write plain spoken sentences")
    if TAG_RE.search(text):
        raise BriefingError(2, "publish_briefing: the text contains <tags>; Alexa refuses markup in a news feed")
    if not text.endswith(SENTENCE_END):
        raise BriefingError(2, "publish_briefing: the text does not end with a complete sentence")
    return text


def config():
    feed_dir = os.environ.get("FUNES_NEWS_FEED_DIR", "").strip()
    public_url = os.environ.get("FUNES_NEWS_PUBLIC_URL", "").strip()
    if not feed_dir or not os.path.isabs(feed_dir):
        raise BriefingError(3, "publish_briefing: FUNES_NEWS_FEED_DIR must be set to an absolute directory")
    if not os.path.isdir(feed_dir):
        raise BriefingError(3, f"publish_briefing: {feed_dir} does not exist (it is created when the funnel is set up)")
    if not public_url.startswith("https://"):
        raise BriefingError(3, "publish_briefing: FUNES_NEWS_PUBLIC_URL must be set to the feed's https URL")
    return Path(feed_dir), public_url


def default_title(now):
    local = now.astimezone()
    return f"Noticias del {local.day} de {MONTHS[local.month - 1]}"


def write_feed(feed_dir, entry):
    fd, tmp = tempfile.mkstemp(prefix=".feed-", suffix=".json", dir=feed_dir)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump([entry], fh, ensure_ascii=False)
            fh.flush()
            os.fsync(fh.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, feed_dir / "feed.json")
    except OSError as err:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise BriefingError(1, f"publish_briefing: could not write the feed: {err}")


def parse_args(argv):
    p = argparse.ArgumentParser(prog="publish_briefing")
    p.add_argument("--text", required=True)
    p.add_argument("--title", default="")
    return p.parse_args(argv)


def perform(argv):
    try:
        try:
            args = parse_args(argv)
        except SystemExit:
            return 2, None
        text = check_text(args.text)
        title = CONTROL_RE.sub("", args.title).strip()[:MAX_TITLE]
        feed_dir, public_url = config()
        now = _now()
        entry = {
            "uid": f"urn:uuid:{uuid.uuid4()}",
            "updateDate": now.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.0Z"),
            "titleText": title or default_title(now),
            "mainText": text,
            "redirectionUrl": public_url,
        }
        write_feed(feed_dir, entry)
        words = len(text.split())
        return 0, {
            "ok": True,
            "chars": len(text),
            "words": words,
            "est_seconds": round(words / WORDS_PER_SECOND),
            "update_date": entry["updateDate"],
            "url": public_url,
        }
    except BriefingError as err:
        print(err, file=sys.stderr)
        return err.code, None


def main(argv=None):
    code, result = perform(sys.argv[1:] if argv is None else argv)
    if result is not None:
        print(json.dumps(result, ensure_ascii=False))
    return code


if __name__ == "__main__":
    sys.exit(main())
