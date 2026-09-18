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
    def test_the_url_comes_from_the_issue(self):
        body = pt.post_body(ISSUE, 1)
        self.assertIn("https://reuters.com/tech/anthropic", body)
        self.assertIn("🤖 Anthropic says Claude hacked three firms.", body)

    def test_the_cta_is_appended(self):
        self.assertTrue(pt.post_body(ISSUE, 1).endswith(
            "📧 Subscribe: email me with subject SUBSCRIBE"))

    def test_no_cta_configured_means_no_cta(self):
        # A publication should not inherit another one's call to action.
        plain = dict(ISSUE)
        del plain["cta"]
        self.assertTrue(pt.post_body(plain, 1).endswith(
            "https://reuters.com/tech/anthropic"))

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


if __name__ == "__main__":
    unittest.main()
