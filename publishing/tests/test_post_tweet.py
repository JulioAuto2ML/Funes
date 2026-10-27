"""Unit tests for post_tweet.py.

The LinkedIn API is not tested — it is one POST, and a test that publishes to a
real profile is not a test. What is tested is the two things that were wrong
before this script moved into the repo:

  It reads the issue JSON. The URL it posts must be the one the publisher
  link-checked, not one re-parsed out of a text file a model typed.

  It refuses when the day's run did not send. Cron fires ten times whether or
  not the morning succeeded, so this check is the only thing standing between a
  blocked run and ten public posts from yesterday's issue.
"""

import json
import unittest
from datetime import date
from pathlib import Path
from contextlib import redirect_stdout
from tempfile import TemporaryDirectory
import io

import post_tweet as pt

DAY = date(2026, 7, 30)

ISSUE = {
    "publication": "ai-pulse",
    "date": "2026-07-30",
    "cta": "📧 Subscribe: email me with subject SUBSCRIBE",
    "items": [
        # The headline is deliberately not a substring of the post text, so the
        # "it is not posted" test below can tell the two apart.
        {"n": 1, "emoji": "🤖", "text": "Anthropic says Claude hacked three firms.",
         "url": "https://reuters.com/tech/anthropic", "headline": "Reuters exclusive"},
        {"n": 2, "emoji": "📉", "text": "OpenAI cuts prices on select models.",
         "url": "https://pymnts.com/openai-price-cuts", "headline": "OpenAI price cuts"},
    ],
}


class Fixture:
    def __init__(self, tmp, status="sent", issue=None, reason=None):
        self.dir = Path(tmp)
        issue = ISSUE if issue is None else issue
        pub, day = issue["publication"], issue["date"]

        issue_path = self.dir / "issues" / pub / f"{day}.json"
        issue_path.parent.mkdir(parents=True, exist_ok=True)
        issue_path.write_text(json.dumps(issue), encoding="utf-8")
        self.issue_path = issue_path

        if status is not None:
            record = {"publication": pub, "date": day, "status": status}
            if reason:
                record["reason"] = reason
            run_path = self.dir / "runs" / pub / f"{day}.json"
            run_path.parent.mkdir(parents=True, exist_ok=True)
            run_path.write_text(json.dumps(record), encoding="utf-8")


class TheRunRecordGate(unittest.TestCase):
    def test_a_sent_run_lets_the_issue_through(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            issue = pt.load_sent_issue(f.dir, "ai-pulse", DAY)
        self.assertEqual(len(issue["items"]), 2)

    def test_no_run_record_refuses(self):
        # The case that used to post yesterday's items: nothing ran today.
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, status=None)
            with self.assertRaises(RuntimeError) as cm:
                pt.load_sent_issue(f.dir, "ai-pulse", DAY)
        self.assertIn("never published", str(cm.exception))

    def test_a_blocked_run_refuses_and_says_why(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, status="blocked", reason="2 broken link(s)")
            with self.assertRaises(RuntimeError) as cm:
                pt.load_sent_issue(f.dir, "ai-pulse", DAY)
        message = str(cm.exception)
        self.assertIn("blocked", message)
        self.assertIn("2 broken link(s)", message)   # the cron log is the diagnosis

    def test_a_dry_run_is_not_a_send(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, status="dry-run")
            with self.assertRaises(RuntimeError):
                pt.load_sent_issue(f.dir, "ai-pulse", DAY)

    def test_a_render_only_run_is_not_a_send(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, status="rendered")
            with self.assertRaises(RuntimeError):
                pt.load_sent_issue(f.dir, "ai-pulse", DAY)

    def test_a_record_without_its_issue_refuses(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            f.issue_path.unlink()
            with self.assertRaises(RuntimeError) as cm:
                pt.load_sent_issue(f.dir, "ai-pulse", DAY)
        self.assertIn("no issue at", str(cm.exception))

    def test_publications_do_not_see_each_others_runs(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            with self.assertRaises(RuntimeError):
                pt.load_sent_issue(f.dir, "security-brief", DAY)


class PostBody(unittest.TestCase):
    def test_the_post_text_comes_from_the_issue(self):
        self.assertIn("🤖 Anthropic says Claude hacked three firms.",
                      pt.post_body(ISSUE, 1))

    def test_the_link_is_not_in_the_body(self):
        # LinkedIn demotes posts that carry an outbound link, so the link goes
        # in the first comment instead (see comment_text).
        self.assertNotIn("http", pt.post_body(ISSUE, 1))
        self.assertNotIn("reuters.com", pt.post_body(ISSUE, 1))

    def test_a_note_after_the_text_points_to_the_first_comment(self):
        body = pt.post_body(ISSUE, 1)
        self.assertIn("🔗 Full article in the first comment 👇", body)
        # between the post text and the call to action
        self.assertLess(body.index("hacked three firms."), body.index("🔗"))
        self.assertLess(body.index("🔗"), body.index("📧 Subscribe"))

    def test_the_note_is_in_the_issues_language(self):
        es = dict(ISSUE, language="es")
        self.assertIn("🔗 Artículo completo en el primer comentario 👇",
                      pt.post_body(es, 1))
        self.assertNotIn("Full article", pt.post_body(es, 1))

    def test_no_cta_still_gets_the_note(self):
        plain = dict(ISSUE); del plain["cta"]
        self.assertTrue(pt.post_body(plain, 1).endswith("in the first comment 👇"))

    def test_the_comment_is_the_issues_url(self):
        # Still looked up, not parsed: it is the URL the harvester fetched and
        # the publisher link-checked.
        self.assertEqual(pt.comment_text(ISSUE, 1), "https://reuters.com/tech/anthropic")
        self.assertEqual(pt.comment_text(ISSUE, 2), "https://pymnts.com/openai-price-cuts")

    def test_the_comment_number_is_checked_like_the_body_s(self):
        for n in (0, 3):
            with self.assertRaises(IndexError):
                pt.comment_text(ISSUE, n)

    def test_the_cta_is_appended(self):
        self.assertTrue(pt.post_body(ISSUE, 1).endswith(
            "📧 Subscribe: email me with subject SUBSCRIBE"))

    def test_no_cta_configured_means_no_cta(self):
        # A publication should not inherit another one's call to action.
        plain = dict(ISSUE)
        del plain["cta"]
        self.assertNotIn("Subscribe", pt.post_body(plain, 1))

    def test_indexing_is_one_based(self):
        self.assertIn("OpenAI cuts prices", pt.post_body(ISSUE, 2))

    def test_an_out_of_range_number_raises(self):
        # post_body itself still refuses; main() is what decides whether that
        # is an error or an ordinary short day (see below).
        for n in (0, 3, 10):
            with self.assertRaises(IndexError):
                pt.post_body(ISSUE, n)

    def test_main_treats_a_slot_past_the_end_as_a_quiet_day(self):
        # publish_issue drops an item no page in the pool supports rather than
        # sinking the whole issue, so an issue can be shorter than the number
        # of cron slots. That must exit 0 with an explanatory line: exiting
        # non-zero would put an ERROR in the log most days and teach us to
        # stop reading it.
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)                       # ISSUE has 2 items
            out = io.StringIO()
            with redirect_stdout(out):
                rc = pt.main_with_args(["7", "--dir", str(f.dir),
                                        "--date", DAY.isoformat()])
            self.assertEqual(rc, 0)
            self.assertIn("Nothing to post", out.getvalue())
            self.assertIn("2 item(s)", out.getvalue())

    def test_main_still_posts_a_slot_within_range(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            out = io.StringIO()
            with redirect_stdout(out):
                rc = pt.main_with_args(["1", "--dir", str(f.dir),
                                        "--date", DAY.isoformat(), "--dry-run"])
            self.assertEqual(rc, 0)
            self.assertIn("DRY RUN", out.getvalue())

    def test_the_headline_is_not_posted(self):
        # It is a link label for the newsletter's HTML; in a post it reads as a
        # fragment glued to the end of a URL, which is how the old .txt parser
        # left it.
        self.assertNotIn("Reuters exclusive", pt.post_body(ISSUE, 1))


PNG = b"\x89PNG\r\n\x1a\n" + b"0" * 64


class FakeResponse:
    def __init__(self, json_body=None, headers=None, status=200):
        self._json, self.headers, self.status_code = json_body or {}, headers or {}, status
        self.text = ""

    def json(self):
        return self._json

    def raise_for_status(self):
        if self.status_code >= 400:
            raise RuntimeError(f"HTTP {self.status_code}")


class FakeRequests:
    """Records every call, answers like LinkedIn's three endpoints."""

    def __init__(self, fail_upload=False, fail_post=False, comment_statuses=(200,)):
        self.calls, self.fail_upload, self.fail_post = [], fail_upload, fail_post
        # One status per comment attempt; the last one repeats.
        self.comment_statuses = list(comment_statuses)

    def get(self, url, **kw):
        self.calls.append(("GET", url))
        return FakeResponse({"sub": "abc"})

    def post(self, url, **kw):
        self.calls.append(("POST", url, kw.get("json")))
        if url.endswith("/comments"):
            status = (self.comment_statuses.pop(0) if len(self.comment_statuses) > 1
                      else self.comment_statuses[0])
            return FakeResponse(status=status)
        if url.endswith("registerUpload"):
            return FakeResponse({"value": {
                "asset": "urn:li:digitalmediaAsset:XYZ",
                "uploadMechanism": {
                    "com.linkedin.digitalmedia.uploading.MediaUploadHttpRequest":
                        {"uploadUrl": "https://upload.example/put"}}}},
                status=500 if self.fail_upload else 200)
        return FakeResponse(headers={"x-restli-id": "urn:li:share:1"},
                            status=500 if self.fail_post else 200)

    def put(self, url, **kw):
        self.calls.append(("PUT", url, kw.get("data")))
        return FakeResponse()

    def comment_calls(self):
        return [c for c in self.calls if c[0] == "POST" and c[1].endswith("/comments")]

    def posted_payload(self):
        return [c for c in self.calls if c[0] == "POST" and c[1].endswith("ugcPosts")][0][2]


class ImageLookup(unittest.TestCase):
    def test_finds_the_slots_image(self):
        with TemporaryDirectory() as tmp:
            d = Path(tmp) / "images" / "ai-pulse" / "2026-07-30"
            d.mkdir(parents=True)
            (d / "2.png").write_bytes(PNG)
            self.assertEqual(pt.image_path(Path(tmp), "ai-pulse", DAY, 2), d / "2.png")

    def test_no_file_means_no_image(self):
        with TemporaryDirectory() as tmp:
            self.assertIsNone(pt.image_path(Path(tmp), "ai-pulse", DAY, 1))

    def test_an_empty_or_non_png_file_means_no_image(self):
        # A render that died half way must not become a broken attachment.
        with TemporaryDirectory() as tmp:
            d = Path(tmp) / "images" / "ai-pulse" / "2026-07-30"
            d.mkdir(parents=True)
            (d / "1.png").write_bytes(b"")
            (d / "2.png").write_bytes(b"<html>not an image</html>")
            self.assertIsNone(pt.image_path(Path(tmp), "ai-pulse", DAY, 1))
            self.assertIsNone(pt.image_path(Path(tmp), "ai-pulse", DAY, 2))


class PostingWithAnImage(unittest.TestCase):
    def setUp(self):
        self.tmp = TemporaryDirectory()
        self.png = Path(self.tmp.name) / "1.png"
        self.png.write_bytes(PNG)

    def tearDown(self):
        self.tmp.cleanup()

    def run_post(self, fake, **kw):
        real, pt.requests = pt.requests, fake
        try:
            with redirect_stdout(io.StringIO()) as out:
                result = pt.post_to_linkedin("hello", "tok", **kw)
            return result, out.getvalue()
        finally:
            pt.requests = real

    def test_text_only_when_there_is_no_image(self):
        fake = FakeRequests()
        self.run_post(fake)
        payload = fake.posted_payload()
        content = payload["specificContent"]["com.linkedin.ugc.ShareContent"]
        self.assertEqual(content["shareMediaCategory"], "NONE")
        self.assertFalse([c for c in fake.calls if c[0] == "PUT"])

    def test_registers_uploads_then_posts_the_asset(self):
        fake = FakeRequests()
        result, _ = self.run_post(fake, image=self.png, title="Reuters exclusive")
        self.assertEqual(result, "urn:li:share:1")
        kinds = [c[0] + " " + c[1].rsplit("/", 1)[-1] for c in fake.calls]
        self.assertEqual(kinds, ["GET userinfo", "POST assets?action=registerUpload",
                                 "PUT put", "POST ugcPosts"])
        put = [c for c in fake.calls if c[0] == "PUT"][0]
        self.assertEqual(put[2], PNG)
        content = fake.posted_payload()["specificContent"]["com.linkedin.ugc.ShareContent"]
        self.assertEqual(content["shareMediaCategory"], "IMAGE")
        self.assertEqual(content["media"][0]["media"], "urn:li:digitalmediaAsset:XYZ")
        self.assertEqual(content["media"][0]["status"], "READY")
        self.assertEqual(content["shareCommentary"]["text"], "hello")

    def test_a_failed_upload_posts_text_only_and_says_so(self):
        fake = FakeRequests(fail_upload=True)
        result, out = self.run_post(fake, image=self.png)
        self.assertEqual(result, "urn:li:share:1")
        content = fake.posted_payload()["specificContent"]["com.linkedin.ugc.ShareContent"]
        self.assertEqual(content["shareMediaCategory"], "NONE")
        self.assertIn("text-only", out)

    def test_a_failed_post_is_not_retried(self):
        # Retrying a post that may have half-gone-through is how a profile gets
        # the same item twice. The error surfaces; cron logs it.
        fake = FakeRequests(fail_post=True)
        with self.assertRaises(RuntimeError):
            self.run_post(fake, image=self.png)
        posts = [c for c in fake.calls if c[0] == "POST" and c[1].endswith("ugcPosts")]
        self.assertEqual(len(posts), 1)


class Commenting(unittest.TestCase):
    URN = "urn:li:share:7517268223457861632"

    def setUp(self):
        self._delays, pt.COMMENT_RETRY_DELAYS = pt.COMMENT_RETRY_DELAYS, (0, 0)

    def tearDown(self):
        pt.COMMENT_RETRY_DELAYS = self._delays

    def run_comment(self, fake):
        real, pt.requests = pt.requests, fake
        try:
            with redirect_stdout(io.StringIO()):
                return pt.post_comment(self.URN, "https://x.example/a", "tok",
                                       "urn:li:person:abc")
        finally:
            pt.requests = real

    def test_comments_on_the_url_encoded_post_as_the_author(self):
        fake = FakeRequests()
        self.run_comment(fake)
        (call,) = fake.comment_calls()
        self.assertTrue(call[1].endswith(
            "/socialActions/urn%3Ali%3Ashare%3A7517268223457861632/comments"))
        self.assertEqual(call[2], {"actor": "urn:li:person:abc",
                                   "message": {"text": "https://x.example/a"}})

    def test_a_transient_failure_is_retried(self):
        # LinkedIn can answer 404 for a post it created a second ago.
        fake = FakeRequests(comment_statuses=(404, 503, 200))
        self.run_comment(fake)
        self.assertEqual(len(fake.comment_calls()), 3)

    def test_a_permanent_failure_is_not_retried(self):
        fake = FakeRequests(comment_statuses=(403,))
        with self.assertRaises(RuntimeError) as cm:
            self.run_comment(fake)
        self.assertEqual(len(fake.comment_calls()), 1)
        self.assertIn("403", str(cm.exception))

    def test_gives_up_after_the_last_retry(self):
        fake = FakeRequests(comment_statuses=(503,))
        with self.assertRaises(RuntimeError):
            self.run_comment(fake)
        self.assertEqual(len(fake.comment_calls()), 3)      # first try + 2 retries


class PostedRecord(unittest.TestCase):
    def test_missing_record_is_empty(self):
        with TemporaryDirectory() as tmp:
            self.assertEqual(pt.load_posted(Path(tmp), "ai-pulse", DAY), {})

    def test_round_trips_and_keeps_publications_apart(self):
        with TemporaryDirectory() as tmp:
            pt.save_posted(Path(tmp), "ai-pulse", DAY, {"1": {"post_id": "urn:a"}})
            self.assertEqual(pt.load_posted(Path(tmp), "ai-pulse", DAY),
                             {"1": {"post_id": "urn:a"}})
            self.assertEqual(pt.load_posted(Path(tmp), "ai-pulse-es", DAY), {})

    def test_a_corrupt_record_is_an_error_not_an_empty_one(self):
        # Treating it as empty would repost every slot of the day.
        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "posted" / "ai-pulse" / f"{DAY}.json"
            path.parent.mkdir(parents=True)
            path.write_text("{not json")
            with self.assertRaises(RuntimeError):
                pt.load_posted(Path(tmp), "ai-pulse", DAY)


class MainPostsThenComments(unittest.TestCase):
    """main_with_args end to end, with LinkedIn faked at the requests module."""

    def setUp(self):
        self.tmp = TemporaryDirectory()
        self.f = Fixture(self.tmp.name)
        (self.f.dir / ".env").write_text("LI_ACCESS_TOKEN=tok\n")
        self._delays, pt.COMMENT_RETRY_DELAYS = pt.COMMENT_RETRY_DELAYS, (0, 0)
        self.real, self.fake = pt.requests, None

    def tearDown(self):
        pt.requests = self.real
        pt.COMMENT_RETRY_DELAYS = self._delays
        self.tmp.cleanup()

    def run_main(self, fake, *extra):
        pt.requests = fake
        out = io.StringIO()
        with redirect_stdout(out):
            rc = pt.main_with_args(["1", "--dir", str(self.f.dir),
                                    "--date", DAY.isoformat(), *extra])
        return rc, out.getvalue()

    def record(self):
        return pt.load_posted(self.f.dir, "ai-pulse", DAY)

    def test_posts_without_the_link_then_comments_it(self):
        fake = FakeRequests()
        rc, _ = self.run_main(fake)
        self.assertEqual(rc, 0)
        text = fake.posted_payload()["specificContent"][
            "com.linkedin.ugc.ShareContent"]["shareCommentary"]["text"]
        self.assertNotIn("reuters.com", text)
        (call,) = fake.comment_calls()
        self.assertEqual(call[2]["message"]["text"], "https://reuters.com/tech/anthropic")
        rec = self.record()["1"]
        self.assertEqual(rec["post_id"], "urn:li:share:1")
        self.assertEqual(rec["comment"], "done")

    def test_the_post_id_is_saved_before_the_comment_is_attempted(self):
        fake = FakeRequests(comment_statuses=(403,))
        rc, out = self.run_main(fake)
        rec = self.record()["1"]
        self.assertEqual(rec["post_id"], "urn:li:share:1")
        self.assertEqual(rec["comment"], "pending")
        self.assertIn("403", rec["comment_error"])
        self.assertEqual(rc, pt.EXIT_COMMENT_FAILED)
        self.assertIn("link", out.lower())

    def test_rerunning_a_posted_slot_comments_but_never_reposts(self):
        self.run_main(FakeRequests(comment_statuses=(403,)))      # post ok, comment fails
        fake = FakeRequests()                                      # LinkedIn recovers
        rc, _ = self.run_main(fake)
        self.assertEqual(rc, 0)
        self.assertFalse([c for c in fake.calls
                          if c[0] == "POST" and c[1].endswith("ugcPosts")])
        self.assertEqual(len(fake.comment_calls()), 1)
        self.assertEqual(self.record()["1"]["comment"], "done")

    def test_rerunning_a_finished_slot_does_nothing(self):
        self.run_main(FakeRequests())
        fake = FakeRequests()
        rc, out = self.run_main(fake)
        self.assertEqual(rc, 0)
        self.assertEqual(fake.calls, [])
        self.assertIn("already posted", out)

    def test_a_later_slot_finishes_an_earlier_slots_missing_comment(self):
        self.run_main(FakeRequests(comment_statuses=(403,)))      # slot 1 pending
        pt.requests = fake = FakeRequests()
        with redirect_stdout(io.StringIO()):
            rc = pt.main_with_args(["2", "--dir", str(self.f.dir),
                                    "--date", DAY.isoformat()])
        self.assertEqual(rc, 0)
        urls = [c[2]["message"]["text"] for c in fake.comment_calls()]
        self.assertEqual(urls, ["https://reuters.com/tech/anthropic",
                                "https://pymnts.com/openai-price-cuts"])
        rec = self.record()
        self.assertEqual((rec["1"]["comment"], rec["2"]["comment"]), ("done", "done"))

    def test_dry_run_shows_the_comment_and_writes_nothing(self):
        fake = FakeRequests()
        rc, out = self.run_main(fake, "--dry-run")
        self.assertEqual(rc, 0)
        self.assertIn("first comment", out)
        self.assertIn("https://reuters.com/tech/anthropic", out)
        self.assertEqual(self.record(), {})
        self.assertEqual(fake.calls, [])


class MainWithImages(unittest.TestCase):
    def test_dry_run_says_whether_an_image_would_be_attached(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            d = f.dir / "images" / "ai-pulse" / "2026-07-30"
            d.mkdir(parents=True)
            (d / "1.png").write_bytes(PNG)
            out = io.StringIO()
            with redirect_stdout(out):
                pt.main_with_args(["1", "--dir", str(f.dir),
                                   "--date", DAY.isoformat(), "--dry-run"])
                pt.main_with_args(["2", "--dir", str(f.dir),
                                   "--date", DAY.isoformat(), "--dry-run"])
        text = out.getvalue()
        self.assertIn("image: ", text)
        self.assertIn("1.png", text)
        self.assertIn("no image", text)


if __name__ == "__main__":
    unittest.main()
