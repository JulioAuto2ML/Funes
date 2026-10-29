#!/usr/bin/env python3
"""
weekly_carousel.py — Sunday's LinkedIn carousel: the week's leading stories.

Usage:
    python3 weekly_carousel.py [--publication ID] [--dir PATH]
                               [--date YYYY-MM-DD] [--per-issue N] [--dry-run]

Since 2026-11-02 each edition publishes three days a week (English Mon/Wed/Fri,
Spanish Tue/Thu/Sat) and Sunday has no issue. Instead each edition gets one
LinkedIn document post — a PDF that LinkedIn shows as a swipeable carousel — with
a cover, one slide per story and a closing slide carrying the CTA.

**Which stories is not a judgement made here.** The curator already put every
issue's items in the order they should appear, so "the most important of the
week" is the first --per-issue items (default 2) of each issue sent in the six
days before the Sunday. No model call: picking again would be a second opinion
on a question already answered, and one more thing that can fail on a Sunday
nobody is watching. An issue whose run record does not say "sent" contributes
nothing, for the same reason post_tweet.py refuses to post from one.

**A carousel is posted at most once.** The post id goes to
<dir>/carousels/<publication>/<date>.json the moment LinkedIn returns it; a
re-run then only finishes the comment with the sources (links go in the first
comment, never the body — LinkedIn demotes a post that carries one).

The PDF is written to <dir>/carousels/<publication>/<date>.pdf, and --dry-run
stops there: render, print, post nothing.

Exit codes:
    0  posted and commented (or, with --dry-run, rendered)
    1  usage/input error, or LinkedIn refused the document or the post
    3  refused: fewer than MIN_STORIES stories in the week
    4  posted, but the sources comment could not be added (re-run to finish)
"""

import json
import sys
import time
from datetime import date, timedelta
from pathlib import Path
from urllib.parse import quote

import requests

SCRIPT_DIR = Path(__file__).parent.resolve()
sys.path.insert(0, str(SCRIPT_DIR))
import post_tweet as pt                              # noqa: E402
from publish_newsletter import issue_dir             # noqa: E402

# Below this the week is too thin to be worth a carousel: one issue's worth.
MIN_STORIES = 3
DEFAULT_PER_ISSUE = 2

# LinkedIn's versioned REST API (documents, posts). A version is served for a
# year after release, so this has to move forward now and then; LI_API_VERSION
# in <dir>/.env overrides it without a deploy.
DEFAULT_API_VERSION = "202609"

# Seconds between checks that an uploaded document has been processed. A post
# that names a document still PROCESSING is refused. Module constant so tests
# don't sleep.
DOCUMENT_POLL_DELAYS = (2, 3, 5, 5, 10, 10, 15, 15, 20, 30)

# 4:5 portrait, what LinkedIn shows largest in the feed.
PAGE_W, PAGE_H = 1080, 1350
MARGIN = 90
INK = (20, 24, 33)
PAPER = (247, 246, 242)
ACCENT = (232, 93, 63)
MUTED = (110, 114, 122)
FONT_DIR = Path("/usr/share/fonts/truetype/dejavu")

COPY = {
    "en": {"kicker": "THE WEEK IN AI", "swipe": "Swipe →",
           "story": "Story {n} of {total}",
           "intro": "🗓️ The week in AI: the {n} stories that led AI Pulse "
                    "this week, in one swipe.",
           "links": "🔗 Sources, in slide order:",
           "close": "That was the week.", "close2": "New issue Monday."},
    "es": {"kicker": "LA SEMANA EN IA", "swipe": "Desliza →",
           "story": "Historia {n} de {total}",
           "intro": "🗓️ La semana en IA: las {n} historias que encabezaron "
                    "AI Pulse en Español esta semana, de un vistazo.",
           "links": "🔗 Fuentes, en el orden de las diapositivas:",
           "close": "Eso fue la semana.", "close2": "Nuevo número el martes."},
}
MONTHS = {"en": ["January", "February", "March", "April", "May", "June", "July",
                 "August", "September", "October", "November", "December"],
          "es": ["enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
                 "agosto", "septiembre", "octubre", "noviembre", "diciembre"]}
WEEKDAYS = {"en": ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"],
            "es": ["lun", "mar", "mié", "jue", "vie", "sáb", "dom"]}


def _copy(language: str) -> dict:
    return COPY.get(language, COPY["en"])


# ── The week ──────────────────────────────────────────────────────────────────

def collect_week(work_dir: Path, publication: str, sunday: date,
                 per_issue: int = DEFAULT_PER_ISSUE) -> list:
    """The leading `per_issue` items of every issue sent in the six days before
    `sunday`, oldest first. Each story carries its day and its illustration (the
    slot's PNG, when generate_images.py made one)."""
    stories = []
    for back in range(6, 0, -1):
        day = sunday - timedelta(days=back)
        try:
            issue = pt.load_sent_issue(work_dir, publication, day)
        except RuntimeError:
            continue                    # no issue that day, or it did not go out
        for slot, item in enumerate(issue.get("items", [])[:per_issue], start=1):
            stories.append({
                "day": day,
                "emoji": item.get("emoji", "").strip(),
                "text": item["text"].strip(),
                "source": item.get("source", ""),
                "url": item["url"],
                "image": pt.image_path(work_dir, publication, day, slot),
            })
    return stories


def date_range(sunday: date, language: str) -> str:
    first, last = sunday - timedelta(days=6), sunday - timedelta(days=1)
    m = MONTHS.get(language, MONTHS["en"])
    if language == "es":
        if first.month == last.month:
            return f"{first.day}–{last.day} de {m[last.month - 1]} de {last.year}"
        return (f"{first.day} de {m[first.month - 1]} – "
                f"{last.day} de {m[last.month - 1]} de {last.year}")
    if first.month == last.month:
        return f"{m[last.month - 1]} {first.day}–{last.day}, {last.year}"
    return f"{m[first.month - 1]} {first.day} – {m[last.month - 1]} {last.day}, {last.year}"


def commentary(issue: dict, stories: list, sunday: date) -> str:
    """The post's text: an intro, the date range, the CTA. No links."""
    language = issue.get("language", "en")
    c = _copy(language)
    intro = c["intro"].format(n=len(stories))
    cta = (issue.get("cta") or "").strip()
    return "\n\n".join(p for p in (intro, f"📅 {date_range(sunday, language)}", cta) if p)


def links_comment(stories: list, language: str) -> str:
    lines = [_copy(language)["links"]]
    lines += [f"{i}. {s['url']}" for i, s in enumerate(stories, start=1)]
    return "\n".join(lines)


# ── The PDF ───────────────────────────────────────────────────────────────────

def _font(bold: bool, size: int):
    from PIL import ImageFont
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    try:
        return ImageFont.truetype(str(FONT_DIR / name), size)
    except OSError:
        return ImageFont.load_default(size)


def _wrap(draw, text: str, font, width: int) -> list:
    lines, line = [], ""
    for word in text.split():
        trial = f"{line} {word}".strip()
        if draw.textlength(trial, font=font) <= width or not line:
            line = trial
        else:
            lines.append(line)
            line = word
    if line:
        lines.append(line)
    return lines


def _fit_text(draw, text: str, width: int, height: int, sizes=(54, 50, 46, 42, 38, 34)):
    """The largest size at which `text` fits the box, and its lines."""
    for size in sizes:
        font = _font(False, size)
        lines = _wrap(draw, text, font, width)
        if len(lines) * int(size * 1.4) <= height:
            return font, lines, int(size * 1.4)
    font = _font(False, sizes[-1])
    lines = _wrap(draw, text, font, width)
    step = int(sizes[-1] * 1.4)
    return font, lines[:max(1, height // step)], step


def _plain(text: str) -> str:
    """`text` without emoji and pictographs: DejaVu has none, and the box it
    draws instead reads as a bug."""
    return " ".join("".join(ch for ch in text if ord(ch) < 0x2190 or ch in "→–—…").split())


def _page():
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (PAGE_W, PAGE_H), PAPER)
    return img, ImageDraw.Draw(img)


def _brand(draw, issue: dict):
    draw.rectangle([0, 0, PAGE_W, 14], fill=ACCENT)
    draw.text((MARGIN, PAGE_H - MARGIN), issue.get("title", "AI Pulse"),
              font=_font(True, 28), fill=MUTED, anchor="ls")


def _cover(issue: dict, stories: list, sunday: date):
    language = issue.get("language", "en")
    c = _copy(language)
    img, draw = _page()
    _brand(draw, issue)
    y = 380
    draw.text((MARGIN, y), c["kicker"], font=_font(True, 40), fill=ACCENT)
    y += 90
    for line in _wrap(draw, issue.get("title", "AI Pulse"), _font(True, 110),
                      PAGE_W - 2 * MARGIN):
        draw.text((MARGIN, y), line, font=_font(True, 110), fill=INK)
        y += 130
    y += 30
    draw.text((MARGIN, y), date_range(sunday, language), font=_font(False, 40), fill=MUTED)
    draw.text((PAGE_W - MARGIN, PAGE_H - MARGIN), c["swipe"], font=_font(True, 40),
              fill=ACCENT, anchor="rs")
    return img


def _story(issue: dict, story: dict, n: int, total: int):
    from PIL import Image
    language = issue.get("language", "en")
    c = _copy(language)
    img, draw = _page()
    _brand(draw, issue)

    top = 60
    art_h = 0
    if story.get("image"):
        try:
            art = Image.open(story["image"]).convert("RGB")
            art_w = PAGE_W - 2 * MARGIN
            art_h = min(560, int(art.height * art_w / art.width))
            # Cover-crop to the box rather than letterbox it.
            scale = max(art_w / art.width, art_h / art.height)
            art = art.resize((int(art.width * scale) + 1, int(art.height * scale) + 1))
            left = (art.width - art_w) // 2
            upper = (art.height - art_h) // 2
            img.paste(art.crop((left, upper, left + art_w, upper + art_h)),
                      (MARGIN, top + 40))
        except Exception:                        # noqa: BLE001 — an image is garnish
            art_h = 0

    y = top + 40 + (art_h + 50 if art_h else 140)
    day = story["day"]
    label = (f"{c['story'].format(n=n, total=total)}  ·  "
             f"{WEEKDAYS.get(language, WEEKDAYS['en'])[day.weekday()]} {day.day}")
    draw.text((MARGIN, y), label.upper(), font=_font(True, 30), fill=ACCENT)
    y += 70

    bottom = PAGE_H - MARGIN - 90
    font, lines, step = _fit_text(draw, _plain(story["text"]), PAGE_W - 2 * MARGIN, bottom - y)
    for line in lines:
        draw.text((MARGIN, y), line, font=font, fill=INK)
        y += step

    if story.get("source"):
        draw.text((PAGE_W - MARGIN, PAGE_H - MARGIN), story["source"],
                  font=_font(False, 28), fill=MUTED, anchor="rs")
    return img


def _closing(issue: dict):
    c = _copy(issue.get("language", "en"))
    img, draw = _page()
    _brand(draw, issue)
    y = 360
    draw.text((MARGIN, y), c["close"], font=_font(True, 72), fill=INK)
    y += 110
    draw.text((MARGIN, y), c["close2"], font=_font(False, 44), fill=MUTED)
    y += 140
    cta = _plain(issue.get("cta") or "")
    font = _font(False, 38)
    for line in _wrap(draw, cta, font, PAGE_W - 2 * MARGIN):
        draw.text((MARGIN, y), line, font=font, fill=INK)
        y += 56
    return img


def render_pdf(issue: dict, stories: list, sunday: date, out: Path) -> int:
    """Writes the carousel to `out`; returns the page count."""
    pages = [_cover(issue, stories, sunday)]
    pages += [_story(issue, s, i, len(stories)) for i, s in enumerate(stories, start=1)]
    pages.append(_closing(issue))
    out.parent.mkdir(parents=True, exist_ok=True)
    pages[0].save(out, "PDF", save_all=True, append_images=pages[1:], resolution=150)
    return len(pages)


# ── LinkedIn ──────────────────────────────────────────────────────────────────

def _rest_headers(token: str, version: str) -> dict:
    return {"Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "LinkedIn-Version": version,
            "X-Restli-Protocol-Version": "2.0.0"}


def upload_document(pdf: Path, token: str, author: str, version: str) -> str:
    """Initializes an upload, PUTs the PDF, waits until LinkedIn has processed
    it. Returns the document URN."""
    r = requests.post(
        "https://api.linkedin.com/rest/documents?action=initializeUpload",
        headers=_rest_headers(token, version),
        json={"initializeUploadRequest": {"owner": author}})
    r.raise_for_status()
    value = r.json()["value"]
    put = requests.put(value["uploadUrl"],
                       headers={"Authorization": f"Bearer {token}"},
                       data=pdf.read_bytes())
    put.raise_for_status()
    urn = value["document"]

    status = "unknown"
    for delay in DOCUMENT_POLL_DELAYS:
        time.sleep(delay)
        g = requests.get(
            f"https://api.linkedin.com/rest/documents/{quote(urn, safe='')}",
            headers=_rest_headers(token, version))
        g.raise_for_status()
        status = g.json().get("status", "unknown")
        if status == "AVAILABLE":
            return urn
        if status == "PROCESSING_FAILED":
            break
    raise RuntimeError(f"document {urn} not available (status {status})")


def post_document(text: str, title: str, document: str, token: str, author: str,
                  version: str) -> str:
    """Creates the post. Never retried: it may have half gone through."""
    r = requests.post(
        "https://api.linkedin.com/rest/posts",
        headers=_rest_headers(token, version),
        json={"author": author,
              "commentary": text,
              "visibility": "PUBLIC",
              "distribution": {"feedDistribution": "MAIN_FEED",
                               "targetEntities": [],
                               "thirdPartyDistributionChannels": []},
              "content": {"media": {"title": title, "id": document}},
              "lifecycleState": "PUBLISHED",
              "isReshareDisabledByAuthor": False})
    r.raise_for_status()
    return r.headers.get("x-restli-id", r.text)


# ── Record ────────────────────────────────────────────────────────────────────

def record_path(work_dir: Path, publication: str, sunday: date) -> Path:
    return work_dir / "carousels" / publication / f"{sunday.isoformat()}.json"


def load_record(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        # Unreadable is not "nothing posted": guessing that is how a carousel
        # goes out twice.
        raise RuntimeError(f"{path} is unreadable ({e}); fix or remove it by hand")


def save_record(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(record, indent=2), encoding="utf-8")
    tmp.replace(path)


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    return main_with_args(sys.argv[1:])


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
    per_issue = take("--per-issue")
    dry_run = "--dry-run" in args
    if [a for a in args if a != "--dry-run"]:
        print("Usage: python3 weekly_carousel.py [--publication ID] [--dir PATH] "
              "[--date YYYY-MM-DD] [--per-issue N] [--dry-run]")
        return 1

    try:
        sunday = date.fromisoformat(day) if day else date.today()
        per_issue = int(per_issue) if per_issue else DEFAULT_PER_ISSUE
    except ValueError as e:
        print(f"ERROR: {e}")
        return 1
    if sunday.weekday() != 6:
        print(f"ERROR: {sunday} is not a Sunday — the carousel covers Monday to Saturday.")
        return 1

    work_dir = issue_dir(explicit_dir)
    print(f"=== weekly_carousel {publication} {sunday} ===")

    stories = collect_week(work_dir, publication, sunday, per_issue)
    if len(stories) < MIN_STORIES:
        print(f"REFUSED: {len(stories)} stor{'y' if len(stories) == 1 else 'ies'} "
              f"from sent {publication} issues this week; a carousel needs "
              f"{MIN_STORIES}.")
        return 3

    # The latest issue of the week carries the current title, language and CTA.
    latest = max(s["day"] for s in stories)
    issue = pt.load_sent_issue(work_dir, publication, latest)
    language = issue.get("language", "en")
    text = commentary(issue, stories, sunday)
    comment = links_comment(stories, language)

    pdf = work_dir / "carousels" / publication / f"{sunday.isoformat()}.pdf"
    pages = render_pdf(issue, stories, sunday, pdf)
    print(f"Rendered {pages} pages ({len(stories)} stories) to {pdf}")
    print(f"\n{text}\n\nfirst comment:\n{comment}\n")
    if dry_run:
        print("[DRY RUN] nothing was posted.")
        return 0

    rec_path = record_path(work_dir, publication, sunday)
    try:
        record = load_record(rec_path)
    except RuntimeError as e:
        print(f"ERROR: {e}")
        return 1
    if record.get("post_id") and record.get("comment") == "done":
        print(f"Already posted ({record['post_id']}) and commented — nothing to do.")
        return 0

    creds = pt.load_env(work_dir / ".env")
    token = pt.get_valid_token(creds)
    version = creds.get("LI_API_VERSION") or DEFAULT_API_VERSION
    author = pt.get_person_urn(token)

    if record.get("post_id"):
        print(f"Already posted ({record['post_id']}); not posting again.")
    else:
        title = f"{_copy(language)['kicker'].capitalize()} · {date_range(sunday, language)}"
        try:
            document = upload_document(pdf, token, author, version)
            post_id = post_document(text, title, document, token, author, version)
        except Exception as e:                        # noqa: BLE001 — reported for cron
            print(f"FAILED: {e}")
            return 1
        # Written before the comment: from here on the carousel is public, and
        # this record is what stops a re-run from making a second.
        record = {"post_id": post_id, "document": document, "comment": "pending",
                  "stories": len(stories),
                  "posted_at": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
        save_record(rec_path, record)
        print(f"Posted the carousel: {post_id}")

    try:
        pt.post_comment(record["post_id"], comment, token, author)
    except RuntimeError as e:
        print(f"WARNING: posted, but the sources comment failed: {e}")
        record.update(comment="pending", comment_error=str(e))
        save_record(rec_path, record)
        return 4
    record.update(comment="done")
    record.pop("comment_error", None)
    save_record(rec_path, record)
    print("Commented the sources.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
