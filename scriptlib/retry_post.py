#!/usr/bin/env python3
"""Retry one of today's LinkedIn posts that did not go out.

--post=PUB/N   e.g. ai-pulse-es/5 (or ai-pulse-es.5, the way the newsletter
               probe names a missing post in its incident key)

Runs publishing/post_tweet.py for that one slot, exactly as cron would have,
and appends what it printed to post_log.txt like cron does. post_tweet.py is
what makes this safe to approve twice: a slot already in today's posted record
is not posted again (only its link comment is finished, if it is owed), and an
issue that was not sent is refused. Today only — a post that missed its day is
not news any more.

A slot whose earlier attempt dropped after sending is "uncertain" (it may be
public) and post_tweet.py refuses it with exit 5; this script has no way to
force it. A person looks at the profile.

Before 7.23 nothing could do this but a person at a terminal: asked to retry a
post, the curator re-ran the harvest and proposed re-sending the whole
newsletter (2026-11-14).

Prints {"ok", "post", "exit_code", "output"}; exits non-zero when the post did
not go out.
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

POST = re.compile(r"^([a-z0-9][a-z0-9-]{0,40})[/.](\d{1,2})$")


def parse_args(argv):
    args = {}
    for token in argv:
        if not token.startswith("--"):
            raise SystemExit(f"unexpected argument: {token}")
        key, sep, value = token[2:].partition("=")
        args[key] = value if sep else True
    return args


def command(root: Path, news_dir: Path, publication: str, slot: int) -> list:
    return [sys.executable, str(root / "publishing" / "post_tweet.py"), str(slot),
            "--dir", str(news_dir), "--publication", publication]


def main(argv) -> int:
    post = str(parse_args(argv).get("post", "")).strip()
    m = POST.match(post)
    if not m:
        raise SystemExit(f"--post must look like ai-pulse-es/5, not {post or '(none)'!r}")
    publication, slot = m.group(1), int(m.group(2))
    root = Path(os.environ.get("FUNES_ROOT", ""))
    news_dir = Path(os.environ.get("NEWSLETTER_DIR", ""))
    if not (root / "publishing" / "post_tweet.py").is_file() or not news_dir.is_dir():
        raise SystemExit("FUNES_ROOT / NEWSLETTER_DIR are not set up for this script")
    r = subprocess.run(command(root, news_dir, publication, slot), capture_output=True,
                       text=True, timeout=240, cwd=str(news_dir))
    out = (r.stdout + r.stderr).strip()
    try:
        with open(news_dir / "post_log.txt", "a", encoding="utf-8") as log:
            log.write(f"[retry_post {post}]\n{out}\n")
    except OSError:
        pass
    tail = out[-1500:]
    json.dump({"ok": r.returncode == 0, "post": post, "exit_code": r.returncode, "output": tail},
              sys.stdout)
    return 0 if r.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
