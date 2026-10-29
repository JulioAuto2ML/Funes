"""Unit tests for weekly_carousel.py.

What is pinned here is what decides *what* goes public on a Sunday: which
stories are picked (only from issues that were sent, only from the week, the
curator's own order), that a thin week is refused rather than padded, and that a
carousel already on record is never posted twice. The LinkedIn calls are faked —
the order of the calls is tested, not LinkedIn.
"""

import io
import json
import unittest
from contextlib import redirect_stdout
from datetime import date
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

import weekly_carousel as wc

SUNDAY = date(2026, 11, 8)
PNG = b"\x89PNG\r\n\x1a\n" + b"\0" * 16

try:
    import PIL  # noqa: F401
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def issue(day, n_items=9, language="en", publication="ai-pulse"):
    return {
        "publication": publication, "date": day, "language": language,
        "title": "AI Pulse", "cta": "📧 Subscribe: email me",
        "items": [{"n": i, "emoji": "🤖", "text": f"{day} story {i}.",
                   "headline": f"Headline {i}", "source": "example.com",
                   "url": f"https://example.com/{day}/{i}"}
                  for i in range(1, n_items + 1)],
    }


class Week:
    """A data dir holding the given days' issues, sent unless told otherwise."""

    def __init__(self, tmp, days, publication="ai-pulse", status="sent",
                 images=(), language="en"):
        self.dir = Path(tmp)
        self.publication = publication
        for day in days:
            p = self.dir / "issues" / publication / f"{day}.json"
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(json.dumps(issue(day, language=language,
                                          publication=publication)),
                         encoding="utf-8")
            r = self.dir / "runs" / publication / f"{day}.json"
            r.parent.mkdir(parents=True, exist_ok=True)
            r.write_text(json.dumps({"status": status}), encoding="utf-8")
        for day, slot in images:
            img = self.dir / "images" / publication / day / f"{slot}.png"
            img.parent.mkdir(parents=True, exist_ok=True)
            img.write_bytes(PNG)


EN_WEEK = ["2026-11-02", "2026-11-04", "2026-11-06"]
ES_WEEK = ["2026-11-03", "2026-11-05", "2026-11-07"]


class CollectTests(unittest.TestCase):

    def test_takes_each_issues_leading_stories_in_date_order(self):
        with TemporaryDirectory() as tmp:
            w = Week(tmp, EN_WEEK)
            stories = wc.collect_week(w.dir, "ai-pulse", SUNDAY, per_issue=2)
        self.assertEqual([s["url"] for s in stories], [
            "https://example.com/2026-11-02/1", "https://example.com/2026-11-02/2",
            "https://example.com/2026-11-04/1", "https://example.com/2026-11-04/2",
            "https://example.com/2026-11-06/1", "https://example.com/2026-11-06/2"])
        self.assertEqual(stories[0]["day"], date(2026, 11, 2))

    def test_ignores_issues_that_were_not_sent(self):
        with TemporaryDirectory() as tmp:
            w = Week(tmp, EN_WEEK)
            (w.dir / "runs" / "ai-pulse" / "2026-11-04.json").write_text(
                json.dumps({"status": "blocked"}))
            stories = wc.collect_week(w.dir, "ai-pulse", SUNDAY, per_issue=2)
        self.assertNotIn(date(2026, 11, 4), {s["day"] for s in stories})
        self.assertEqual(len(stories), 4)

    def test_only_the_six_days_before_the_sunday(self):
        with TemporaryDirectory() as tmp:
            w = Week(tmp, ["2026-11-01", "2026-10-30"] + EN_WEEK + ["2026-11-08"])
            stories = wc.collect_week(w.dir, "ai-pulse", SUNDAY, per_issue=1)
        self.assertEqual({s["day"].isoformat() for s in stories}, set(EN_WEEK))

    def test_finds_the_days_illustration(self):
        with TemporaryDirectory() as tmp:
            w = Week(tmp, EN_WEEK, images=[("2026-11-04", 2)])
            stories = wc.collect_week(w.dir, "ai-pulse", SUNDAY, per_issue=2)
        with_image = [s for s in stories if s["image"]]
        self.assertEqual(len(with_image), 1)
        self.assertEqual(with_image[0]["url"], "https://example.com/2026-11-04/2")


class TextTests(unittest.TestCase):

    def stories(self):
        return [{"day": date(2026, 11, 2), "emoji": "🤖", "text": "A.",
                 "source": "x.com", "url": "https://x.com/a", "image": None},
                {"day": date(2026, 11, 4), "emoji": "📉", "text": "B.",
                 "source": "y.com", "url": "https://y.com/b", "image": None}]

    def test_commentary_has_no_links_and_ends_with_the_cta(self):
        text = wc.commentary(issue("2026-11-06"), self.stories(), SUNDAY)
        self.assertNotIn("https://", text)
        self.assertTrue(text.endswith("📧 Subscribe: email me"))

    def test_commentary_is_in_the_editions_language(self):
        es = issue("2026-11-07", language="es", publication="ai-pulse-es")
        self.assertIn("semana", wc.commentary(es, self.stories(), SUNDAY).lower())

    def test_links_comment_lists_every_source_once_numbered(self):
        text = wc.links_comment(self.stories(), "en")
        self.assertIn("1. https://x.com/a", text)
        self.assertIn("2. https://y.com/b", text)


@unittest.skipUnless(HAVE_PIL, "Pillow not installed")
class RenderTests(unittest.TestCase):

    def test_one_page_per_story_plus_cover_and_closing(self):
        with TemporaryDirectory() as tmp:
            w = Week(tmp, EN_WEEK, images=[("2026-11-02", 1)])
            stories = wc.collect_week(w.dir, "ai-pulse", SUNDAY, per_issue=2)
            out = w.dir / "c.pdf"
            # A fake PNG header is not an image; render must fall back, not die.
            pages = wc.render_pdf(issue("2026-11-06"), stories, SUNDAY, out)
            self.assertEqual(pages, len(stories) + 2)
            self.assertTrue(out.read_bytes().startswith(b"%PDF"))


class FakeResponse:
    def __init__(self, status=200, body=None, headers=None):
        self.status_code = status
        self._body = body or {}
        self.headers = headers or {}
        self.text = json.dumps(self._body)

    def json(self):
        return self._body

    def raise_for_status(self):
        if self.status_code >= 400:
            raise RuntimeError(f"HTTP {self.status_code}")


def run(args):
    out = io.StringIO()
    with redirect_stdout(out):
        rc = wc.main_with_args(list(args))
    return rc, out.getvalue()


class MainTests(unittest.TestCase):

    def setUp(self):
        wc.DOCUMENT_POLL_DELAYS = (0,)
        self.render = mock.patch.object(
            wc, "render_pdf",
            side_effect=lambda iss, st, day, out: (out.parent.mkdir(parents=True, exist_ok=True),
                                                   out.write_bytes(b"%PDF-1.4"),
                                                   len(st) + 2)[-1])
        self.render.start()
        self.addCleanup(self.render.stop)

    def test_thin_week_is_refused(self):
        with TemporaryDirectory() as tmp:
            Week(tmp, EN_WEEK[:1])
            rc, out = run(["--dir", tmp, "--date", "2026-11-08"])
        self.assertEqual(rc, 3)
        self.assertIn("REFUSED", out)

    def test_not_a_sunday_is_a_usage_error(self):
        with TemporaryDirectory() as tmp:
            Week(tmp, EN_WEEK)
            rc, _ = run(["--dir", tmp, "--date", "2026-11-07"])
        self.assertEqual(rc, 1)

    def test_dry_run_renders_and_posts_nothing(self):
        with TemporaryDirectory() as tmp, \
                mock.patch.object(wc.requests, "post") as post:
            Week(tmp, EN_WEEK)
            rc, out = run(["--dir", tmp, "--date", "2026-11-08", "--dry-run"])
            self.assertTrue((Path(tmp) / "carousels" / "ai-pulse" / "2026-11-08.pdf").exists())
        self.assertEqual(rc, 0)
        post.assert_not_called()
        self.assertIn("DRY RUN", out)

    def fake_linkedin(self, calls):
        def post(url, **kw):
            calls.append(("POST", url))
            if "initializeUpload" in url:
                return FakeResponse(body={"value": {
                    "uploadUrl": "https://upload.example/doc",
                    "document": "urn:li:document:D1"}})
            if url.endswith("/rest/posts"):
                return FakeResponse(201, headers={"x-restli-id": "urn:li:share:P1"})
            return FakeResponse(201)                 # the comment

        def get(url, **kw):
            calls.append(("GET", url))
            if "userinfo" in url:
                return FakeResponse(body={"sub": "ME"})
            return FakeResponse(body={"status": "AVAILABLE"})

        def put(url, **kw):
            calls.append(("PUT", url))
            return FakeResponse(201)
        return post, get, put

    def test_posts_document_then_records_then_comments(self):
        calls = []
        post, get, put = self.fake_linkedin(calls)
        with TemporaryDirectory() as tmp, \
                mock.patch.object(wc.requests, "post", side_effect=post), \
                mock.patch.object(wc.pt.requests, "post", side_effect=post), \
                mock.patch.object(wc.pt.requests, "get", side_effect=get), \
                mock.patch.object(wc.requests, "get", side_effect=get), \
                mock.patch.object(wc.requests, "put", side_effect=put):
            Week(tmp, EN_WEEK)
            (Path(tmp) / ".env").write_text("LI_ACCESS_TOKEN=t\n")
            rc, out = run(["--dir", tmp, "--date", "2026-11-08"])
            record = json.loads((Path(tmp) / "carousels" / "ai-pulse" /
                                 "2026-11-08.json").read_text())
        self.assertEqual(rc, 0, out)
        def label(url):
            if "initializeUpload" in url:
                return "init"
            if url.startswith("https://upload.example"):
                return "upload"
            if "/rest/documents/" in url:
                return "status"
            if url.endswith("/rest/posts"):
                return "post"
            if url.endswith("/comments"):
                return "comment"
            return None
        order = [label(u) for _, u in calls if label(u)]
        # The document must be AVAILABLE before the post that shows it, and the
        # record is written before the comment (asserted via record below).
        self.assertEqual(order, ["init", "upload", "status", "post", "comment"])
        self.assertEqual(record["post_id"], "urn:li:share:P1")
        self.assertEqual(record["comment"], "done")

    def test_a_carousel_on_record_is_never_posted_again(self):
        calls = []
        post, get, put = self.fake_linkedin(calls)
        with TemporaryDirectory() as tmp, \
                mock.patch.object(wc.requests, "post", side_effect=post), \
                mock.patch.object(wc.pt.requests, "post", side_effect=post), \
                mock.patch.object(wc.pt.requests, "get", side_effect=get), \
                mock.patch.object(wc.requests, "get", side_effect=get), \
                mock.patch.object(wc.requests, "put", side_effect=put):
            Week(tmp, EN_WEEK)
            (Path(tmp) / ".env").write_text("LI_ACCESS_TOKEN=t\n")
            rec = Path(tmp) / "carousels" / "ai-pulse" / "2026-11-08.json"
            rec.parent.mkdir(parents=True)
            rec.write_text(json.dumps({"post_id": "urn:li:share:OLD",
                                       "comment": "pending"}))
            rc, out = run(["--dir", tmp, "--date", "2026-11-08"])
            record = json.loads(rec.read_text())
        self.assertEqual(rc, 0, out)
        self.assertFalse(any("rest/posts" in u or "documents" in u for _, u in calls))
        self.assertEqual(record["post_id"], "urn:li:share:OLD")
        self.assertEqual(record["comment"], "done")


if __name__ == "__main__":
    unittest.main()
