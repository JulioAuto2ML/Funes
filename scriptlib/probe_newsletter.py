#!/usr/bin/env python3
"""Self-healing probe (H0): the newsletter's LinkedIn side.

Two checks, both from 2026-11-12/13, when the LinkedIn token expired and every
post failed for a day with the error only in post_log.txt:

  linkedin_token  — the token in data/newsletter/.env has more than two days
                    left. The daily `linkedin_token.py check` already sends the
                    renewal link from a week out; this is the alarm for when
                    that link was not used.
  post.<pub>.<n>   — one per post slot whose time has passed today (from the
                    crontab's post_tweet.py lines): is it in the posted record?
                    (One aggregate `linkedin_posts` check until 7.23; one per
                    post since, so an incident names the post to retry.)
  post_unsure.<pub>.<n> — the post request dropped after it was sent
                    (post_tweet.py exit 5): it may be public. A person looks.
                    A slot past its item count, or whose item is withdrawn,
                    is not owed.

Read-only. No model.
"""

import json
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

TOKEN_WARN_DAYS = 2
# A slot is owed this long after its cron time: post_tweet.py renders an
# image and posts twice (the post, then the link comment).
GRACE_MINUTES = 20

CRON_POST = re.compile(r"^\s*(\d+)\s+(\d+)\s+\*\s+\*\s+([\d,*-]+)\s+.*post_tweet\.py\s+(\d+)\b(.*)$")


def load_env(path: Path) -> dict:
    out = {}
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    return out


def token_check(news_dir: Path, now: float):
    expiry = load_env(news_dir / ".env").get("LI_TOKEN_EXPIRY", "")
    if not expiry.isdigit():
        return None
    days = (int(expiry) - now) / 86400
    when = datetime.fromtimestamp(int(expiry)).strftime("%Y-%m-%d %H:%M")
    if days > TOKEN_WARN_DAYS:
        return {"name": "linkedin_token", "ok": True,
                "summary": f"The LinkedIn token is valid until {when}"}
    expired = days <= 0
    return {"name": "linkedin_token", "ok": False,
            "severity": "critical" if expired else "warning",
            "summary": (f"The LinkedIn token expired on {when}: every post fails until it is renewed"
                        if expired else
                        f"The LinkedIn token expires on {when} ({days:.1f} days)"),
            "evidence": "Renew it from the link `linkedin_token.py check` sends on WhatsApp, "
                        "or run `publishing/linkedin_token.py url` on yoda."}


def cron_dow_matches(field: str, dow: int) -> bool:
    """dow: 0 = Sunday, as cron counts."""
    if field == "*":
        return True
    for part in field.split(","):
        if "-" in part:
            a, b = part.split("-", 1)
            if a.isdigit() and b.isdigit() and int(a) <= dow <= int(b):
                return True
        elif part.isdigit() and int(part) % 7 == dow:
            return True
    return False


def post_slots(crontab: str, dow: int) -> list:
    """[(publication, slot, minute_of_day)] for today's post_tweet.py lines."""
    out = []
    for line in crontab.splitlines():
        m = CRON_POST.match(line)
        if not m or not cron_dow_matches(m.group(3), dow):
            continue
        pub = re.search(r"--publication\s+(\S+)", m.group(5))
        out.append((pub.group(1) if pub else "ai-pulse", int(m.group(4)),
                    int(m.group(2)) * 60 + int(m.group(1))))
    return out


def last_errors(log: Path, n=3) -> str:
    try:
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()[-200:]
    except OSError:
        return ""
    errs = [l for l in lines if l.startswith("ERROR") or "Traceback" in l or "Error:" in l]
    return "\n".join(errs[-n:])


def posts_check(news_dir: Path, crontab: str, now: datetime):
    today = now.strftime("%Y-%m-%d")
    minute = now.hour * 60 + now.minute
    dow = (now.weekday() + 1) % 7
    owed = {}
    for pub, slot, at in post_slots(crontab, dow):
        if at + GRACE_MINUTES > minute:
            continue
        run = news_dir / "runs" / pub / f"{today}.json"
        issue = news_dir / "issues" / pub / f"{today}.json"
        try:
            if json.loads(run.read_text(encoding="utf-8")).get("status") != "sent":
                continue   # no issue went out: the curator job's failure says so
            items = json.loads(issue.read_text(encoding="utf-8")).get("items", [])
        except (OSError, ValueError):
            continue
        if slot > len(items) or items[slot - 1].get("withdrawn"):
            continue
        owed.setdefault(pub, []).append(slot)
    # One check per post owed (7.23): an incident a retry can name. The key is
    # probe_newsletter:post.<publication>.<slot>, and heal.yaml proposes
    # retry_post for it; a post that went out reports ok, which closes it.
    checks = []
    log = news_dir / "post_log.txt"
    for pub, slots in sorted(owed.items()):
        try:
            posted = json.loads((news_dir / "posted" / pub / f"{today}.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            posted = {}
        for s in sorted(slots):
            name = f"post.{pub}.{s}"
            entry = posted.get(str(s), {})
            if not entry.get("post_id") and entry.get("uncertain"):
                # The request dropped after it was sent (post_tweet.py exit 5):
                # it may be public. Its own key, so heal.yaml explains instead
                # of proposing a retry that would post it twice (2026-11-14).
                checks.append({"name": f"post_unsure.{pub}.{s}", "ok": False, "severity": "warning",
                               "summary": f"Today's LinkedIn post {pub} #{s} may or may not have gone out",
                               "evidence": entry["uncertain"]})
                continue
            if entry.get("post_id"):
                checks.append({"name": name, "ok": True,
                               "summary": f"LinkedIn post {pub} #{s} went out today"})
            else:
                checks.append({"name": name, "ok": False, "severity": "warning",
                               "summary": f"Today's LinkedIn post {pub} #{s} did not go out",
                               "evidence": last_errors(log) or "nothing in post_log.txt"})
    return checks


def collect(news_dir: Path, crontab_file: Path, now: float) -> list:
    checks = [token_check(news_dir, now)]
    try:
        crontab = crontab_file.read_text(encoding="utf-8")
    except OSError:
        crontab = ""
    if crontab:
        checks += posts_check(news_dir, crontab, datetime.fromtimestamp(now))
    return [c for c in checks if c]


def main() -> int:
    news_dir = Path(os.environ.get("NEWSLETTER_DIR", ""))
    crontab = Path(os.environ.get("CRONTAB_FILE", ""))
    checks = collect(news_dir, crontab, time.time()) if news_dir.is_dir() else []
    json.dump({"checks": checks}, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
