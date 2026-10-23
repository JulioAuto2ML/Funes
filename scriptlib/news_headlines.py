#!/usr/bin/env python3
"""news_headlines — the day's Argentine and world headlines, ranked by coverage.

Run by Funes through `run_script` (see news_headlines.yaml); stdout is exactly
one JSON object, everything else goes to stderr. Standard library only.

    [--hours=18] [--max_items=20]

Reads Infobae, La Nación and Clarín (FEEDS below; the list is the admin's to
edit, not the model's to supply) and returns the recent items as one list.

What this script decides for itself, so the model writing the briefing does not
have to — and cannot get wrong:

- **What is recent.** Items older than `--hours` are dropped; an item with no
  readable date is dropped too, rather than treated as new.
- **What is the same story.** Items from *different* outlets whose titles share
  most of their content words are one story with several sources. Items from
  the same outlet are never merged: an outlet does not publish one story twice,
  and the merge exists to count outlets.
- **What matters most.** A story covered by three outlets outranks one only one
  outlet has, however much newer that one is. Ties go to the newest. This is
  the editorial judgement, and it is made here, in code.
- **What is not news.** SKIP_CATEGORIES (horoscopes, recipes, games…) are left
  out wherever a feed labels them.

A feed that fails or returns malformed XML is reported in `errors` and the rest
are still used; every feed failing is exit 1.

Exit codes: 0 done, 1 no feed could be read, 2 bad arguments.
"""

import argparse
import html
import json
import re
import sys
import unicodedata
import urllib.request
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from email.utils import parsedate_to_datetime


@dataclass(frozen=True)
class Feed:
    source: str
    section: object  # str or None when the feed labels its own categories
    url: str


INFOBAE = "https://www.infobae.com/arc/outboundfeeds/rss/category/{}/?outputType=xml"
FEEDS = [
    Feed("Infobae", "Política", INFOBAE.format("politica")),
    Feed("Infobae", "Economía", INFOBAE.format("economia")),
    Feed("Infobae", "Sociedad", INFOBAE.format("sociedad")),
    Feed("La Nación", None, "https://www.lanacion.com.ar/arc/outboundfeeds/rss/?outputType=xml"),
    Feed("Clarín", "Lo último", "https://www.clarin.com/rss/lo-ultimo/"),
    Feed("Clarín", "Política", "https://www.clarin.com/rss/politica/"),
    Feed("Clarín", "Economía", "https://www.clarin.com/rss/economia/"),
    Feed("Clarín", "Mundo", "https://www.clarin.com/rss/mundo/"),
]

SKIP_CATEGORIES = {"horoscopo", "recetas", "juegos", "clasificados", "funebres", "loterias", "quiniela"}
SUMMARY_CHARS = 200
FETCH_TIMEOUT_S = 12
FETCH_MAX_BYTES = 4 * 1024 * 1024
USER_AGENT = "Mozilla/5.0 (compatible; FunesNews/1.0)"
MIN_SHARED_WORDS = 3
MIN_OVERLAP = 0.6

STOPWORDS = {
    "the", "con", "por", "para", "una", "uno", "unos", "unas", "los", "las", "del", "que", "como", "mas",
    "sus", "son", "fue", "ser", "esta", "este", "esto", "hoy", "ante", "tras", "sobre", "entre", "sin",
    "pero", "cual", "quien", "donde", "cuando", "muy", "ya", "les", "nos", "hay", "asi", "todo", "todos",
}


@dataclass
class Item:
    title: str
    summary: str
    published: datetime
    source: str
    section: object
    words: frozenset = field(default_factory=frozenset)


def _now():
    return datetime.now(timezone.utc)


def _fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT_S) as resp:
        return resp.read(FETCH_MAX_BYTES)


def _fold(text):
    """Lowercase, accents removed: 'Política' and 'politica' compare equal."""
    decomposed = unicodedata.normalize("NFKD", text.lower())
    return "".join(c for c in decomposed if not unicodedata.combining(c))


def content_words(title):
    tokens = re.findall(r"[a-z0-9]+", _fold(title))
    return frozenset(t for t in tokens if (len(t) >= 3 or t.isdigit()) and t not in STOPWORDS)


def clean_summary(raw):
    text = html.unescape(re.sub(r"<[^>]+>", " ", raw or ""))
    text = re.sub(r"\s+", " ", text.replace("\xa0", " ")).strip()
    if len(text) <= SUMMARY_CHARS:
        return text
    cut = text[:SUMMARY_CHARS].rsplit(" ", 1)[0].rstrip(" ,;:.")
    return cut + "…"


def _published(node):
    raw = node.findtext("pubDate")
    if not raw:
        return None
    try:
        when = parsedate_to_datetime(raw.strip())
    except (TypeError, ValueError):
        return None
    return when if when.tzinfo else when.replace(tzinfo=timezone.utc)


def parse_feed(data, feed):
    root = ET.fromstring(data)
    items = []
    for node in root.iter("item"):
        title = re.sub(r"\s+", " ", html.unescape(node.findtext("title") or "")).strip()
        when = _published(node)
        if not title or when is None:
            continue
        categories = [_fold(c.text or "").strip() for c in node.findall("category")]
        if any(c in SKIP_CATEGORIES for c in categories):
            continue
        section = feed.section or ((node.findtext("category") or "").strip() or None)
        items.append(Item(title, clean_summary(node.findtext("description")), when,
                          feed.source, section, content_words(title)))
    return items


def same_story(a, b):
    shared = len(a.words & b.words)
    smaller = min(len(a.words), len(b.words)) or 1
    return shared >= MIN_SHARED_WORDS and shared / smaller >= MIN_OVERLAP


def cluster(items):
    """Groups of one story across outlets, newest item first in each group."""
    groups = []
    for it in sorted(items, key=lambda i: i.published, reverse=True):
        for group in groups:
            if it.source not in {g.source for g in group} and any(same_story(it, g) for g in group):
                group.append(it)
                break
        else:
            groups.append([it])
    return groups


def rank(groups):
    def key(group):
        return (len({g.source for g in group}), group[0].published)
    return sorted(groups, key=key, reverse=True)


def to_json(group):
    lead = group[0]
    sources = []
    for g in group:
        if g.source not in sources:
            sources.append(g.source)
    return {
        "title": lead.title,
        "summary": lead.summary,
        "section": lead.section,
        "sources": sources,
        "coverage": len(sources),
        "published": lead.published.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%MZ"),
    }


def _read(feed):
    try:
        return feed, parse_feed(_fetch(feed.url), feed), None
    except ET.ParseError as err:
        return feed, [], f"malformed feed: {err}"
    except Exception as err:  # network, HTTP status, timeout: this feed only
        return feed, [], f"{type(err).__name__}: {err}"


def parse_args(argv):
    p = argparse.ArgumentParser(prog="news_headlines")
    p.add_argument("--hours", type=int, default=18)
    p.add_argument("--max_items", type=int, default=20)
    return p.parse_args(argv)


def perform(argv):
    try:
        args = parse_args(argv)
    except SystemExit:
        return 2, None
    if not 1 <= args.hours <= 48:
        print("news_headlines: --hours must be between 1 and 48", file=sys.stderr)
        return 2, None
    if not 1 <= args.max_items <= 40:
        print("news_headlines: --max_items must be between 1 and 40", file=sys.stderr)
        return 2, None

    with ThreadPoolExecutor(max_workers=len(FEEDS)) as pool:
        results = list(pool.map(_read, FEEDS))

    errors = [{"source": f.source, "section": f.section, "error": e} for f, _, e in results if e]
    for err in errors:
        print(f"news_headlines: {err['source']} {err['section'] or ''}: {err['error']}", file=sys.stderr)
    if len(errors) == len(FEEDS):
        print("news_headlines: no feed could be read", file=sys.stderr)
        return 1, None

    since = _now() - timedelta(hours=args.hours)
    recent = [i for _, items, _ in results for i in items if since <= i.published <= _now() + timedelta(hours=1)]
    groups = rank(cluster(recent))[: args.max_items]
    return 0, {
        "ok": True,
        "window_hours": args.hours,
        "items": [to_json(g) for g in groups],
        "errors": errors,
    }


def main(argv=None):
    code, result = perform(sys.argv[1:] if argv is None else argv)
    if result is not None:
        print(json.dumps(result, ensure_ascii=False))
    return code


if __name__ == "__main__":
    sys.exit(main())
