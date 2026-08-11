"""Unit tests for publish_issue.py.

The point of this script is that the issue JSON is the source of truth and the
.txt and the HTML are generated from it. So the tests are mostly about what
comes out the other end: that the URL in every artifact is the one in the JSON,
that the posts file is still shaped the way post_tweet.py and a human expect,
and that a run record lands on every outcome — including the ones where nothing
was sent, which is exactly when someone will go looking for it.

Link checking is stubbed. Whether a given URL resolves is not this script's
decision; what to do about the answer is.
"""

import io
import json
import unittest
from contextlib import redirect_stdout
from datetime import date
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

import publish_issue as pi
import publish_newsletter as pn

DAY = date(2026, 7, 30)

ISSUE = {
    "publication": "ai-pulse",
    "date": "2026-07-30",
    "harvested": 25,
    "items": [
        {"n": 1, "emoji": "🤖", "text": "Anthropic says Claude hacked three firms.",
         "url": "https://reuters.com/tech/anthropic?x=1&y=2",
         "headline": "Claude hacked three firms", "source": "reuters.com",
         "evidence": "hacked into three organisations", "candidate_id": 4},
        {"n": 2, "emoji": "📉", "text": "OpenAI cuts prices on select models.",
         "url": "https://pymnts.com/openai-price-cuts",
         "headline": "OpenAI price cuts", "source": "pymnts.com",
         "evidence": "cuts prices on select models", "candidate_id": 9},
    ],
}


def _link_map(broken=(), suspect=()):
    """A check_link stand-in whose answer depends on which URL is asked —
    real repair tests need item 1's original link to fail while a
    replacement candidate's link succeeds, which a single fixed return
    value (what Fixture.run uses) can't express."""
    def check(url):
        if url in broken:
            return "broken", "HTTP 404"
        if url in suspect:
            return "suspect", "HTTP 403"
        return "ok", "200"
    return check


class Fixture:
    """A workspace with one issue in it, plus the template."""

    def __init__(self, tmp, issue=None, pool=None):
        self.dir = Path(tmp)
        (self.dir / pn.TEMPLATE_NAME).write_text("<p>{{DATE}}</p>\n  {{POSTS}}\n")
        issue = ISSUE if issue is None else issue
        path = self.dir / "issues" / issue["publication"] / f"{issue['date']}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(issue), encoding="utf-8")
        if pool is not None:
            pool_path = self.dir / f"harvest_{issue['publication']}_{issue['date']}.json"
            pool_path.write_text(json.dumps(pool), encoding="utf-8")

    def run(self, send=False, dry_run=False, link=("ok", "200"), sender=None):
        with mock.patch.object(pi, "check_link", return_value=link):
            with mock.patch.object(pi.subprocess, "run") as proc:
                proc.return_value = sender or mock.Mock(
                    returncode=0, stdout="  ✓ Sent to a@b.com\n", stderr="")
                with redirect_stdout(io.StringIO()) as out:
                    code = pi.publish(DAY, "ai-pulse", self.dir, send, dry_run)
        return code, out.getvalue()

    def record(self):
        return json.loads((self.dir / "runs" / "ai-pulse" / "2026-07-30.json").read_text())

    def posts(self):
        return (self.dir / "X_posts_2026-07-30.txt").read_text(encoding="utf-8")

    def html(self):
        return (self.dir / "newsletter_2026-07-30.html").read_text(encoding="utf-8")


class Artifacts(unittest.TestCase):
    def test_posts_file_is_generated_from_the_issue(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            code, _ = f.run()
            posts = f.posts()
        self.assertEqual(code, 0)
        self.assertIn("1. 🤖 Anthropic says Claude hacked three firms.", posts)
        self.assertIn("https://reuters.com/tech/anthropic?x=1&y=2 — Claude hacked three firms",
                      posts)
        self.assertIn("---", posts)

    def test_the_generated_posts_file_parses_back(self):
        # publish_newsletter.py and post_tweet.py both read this file. Round-
        # tripping it through the older parser is what keeps them working while
        # they still exist.
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            f.run()
            parsed = pn.parse_posts(f.posts())
        self.assertEqual(len(parsed), 2)
        self.assertEqual(parsed[0]["url"], ISSUE["items"][0]["url"])
        self.assertEqual(parsed[1]["headline"], "OpenAI price cuts")

    def test_html_carries_the_issue_urls_verbatim(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            f.run()
            html = f.html()
        # &amp; because the href is escaped, which is the correct rendering of
        # the same URL.
        self.assertIn("https://reuters.com/tech/anthropic?x=1&amp;y=2", html)
        self.assertIn("Claude hacked three firms", html)
        self.assertIn("Thursday, July 30, 2026", html)

    def test_emoji_reaches_the_newsletter_body(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            f.run()
            self.assertIn("🤖 Anthropic says", f.html())


class RunRecords(unittest.TestCase):
    def test_a_send_is_recorded(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            code, _ = f.run(send=True)
            record = f.record()
        self.assertEqual(code, 0)
        self.assertEqual(record["status"], "sent")
        self.assertEqual(record["selected"], 2)
        self.assertEqual(record["harvested"], 25)
        self.assertEqual(record["recipients"], 1)
        self.assertEqual(record["links"], {"ok": 2, "suspect": 0, "broken": 0})
        self.assertIn("duration_s", record)

    def test_a_render_only_run_is_not_recorded_as_sent(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            f.run(send=False)
            self.assertEqual(f.record()["status"], "rendered")

    def test_a_dry_run_is_not_recorded_as_sent(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            f.run(send=True, dry_run=True)
            self.assertEqual(f.record()["status"], "dry-run")

    def test_broken_links_with_no_pool_to_repair_from_block_the_send(self):
        # No harvest_*.json in the fixture, so there is nothing to repair
        # from — every broken item is dropped, and with 0 of 2 items left
        # (default min_items 1 still isn't met), the issue is blocked.
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            with mock.patch.object(pi, "check_link", return_value=("broken", "HTTP 404")):
                with mock.patch.object(pi.subprocess, "run") as proc:
                    with redirect_stdout(io.StringIO()) as out:
                        code = pi.publish(DAY, "ai-pulse", f.dir, send=True)
            record = f.record()
        self.assertEqual(code, 2)
        proc.assert_not_called()
        self.assertIn("Nothing was sent", out.getvalue())
        self.assertEqual(record["status"], "blocked")
        self.assertEqual(record["links"]["broken"], 2)
        self.assertEqual(len(record["dropped"]), 2)

    def test_suspect_links_are_recorded_but_do_not_block(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            code, out = f.run(send=True, link=("suspect", "HTTP 403"))
            record = f.record()
        self.assertEqual(code, 0)
        self.assertEqual(record["status"], "sent")
        self.assertEqual(record["links"]["suspect"], 2)
        self.assertIn("SUSPECT", out)

    def test_a_failed_send_is_recorded_as_blocked(self):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            code, out = f.run(send=True, sender=mock.Mock(
                returncode=3, stdout="", stderr="smtp died"))
            record = f.record()
        self.assertEqual(code, 3)
        self.assertEqual(record["status"], "blocked")
        self.assertIn("exited 3", record["reason"])
        self.assertIn("NOT sent", out)

    def test_a_missing_issue_is_recorded_not_just_printed(self):
        # The case a scheduled run hits when the curator never got that far.
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            (f.dir / "issues" / "ai-pulse" / "2026-07-30.json").unlink()
            code, out = f.run(send=True)
            record = f.record()
        self.assertEqual(code, 1)
        self.assertEqual(record["status"], "blocked")
        self.assertIn("no issue at", record["reason"])


class LinkRepair(unittest.TestCase):
    """A broken link at send time used to come back to curator as a tool
    rejection, asking a model to notice and swap the item — unreliably. This
    is the deterministic replacement (2026-08-11): a broken link is repaired
    from an already-harvested same-story candidate, or the item is dropped,
    entirely in code."""

    ANTHROPIC_URL = ISSUE["items"][0]["url"]
    OPENAI_URL = ISSUE["items"][1]["url"]
    ALTERNATE_URL = "https://apnews.com/anthropic-claude-hack"

    POOL_WITH_ALTERNATE = {
        "publication": "ai-pulse", "date": "2026-07-30",
        "candidates": [
            {"id": 4, "story": 1, "title": "Anthropic hack - Reuters",
             "url": ANTHROPIC_URL, "source": "reuters.com",
             "published": "2026-07-29", "result_id": 100, "text": "irrelevant here"},
            {"id": 15, "story": 1, "title": "Anthropic Claude hack coverage - AP News",
             "url": ALTERNATE_URL, "source": "apnews.com", "published": "2026-07-29",
             "result_id": 101,
             "text": "Anthropic confirmed Thursday that its Claude model had hacked "
                     "into three separate organisations during a security review."},
            {"id": 9, "story": 2, "title": "OpenAI price cuts - PYMNTS",
             "url": OPENAI_URL, "source": "pymnts.com",
             "published": "2026-07-29", "result_id": 102, "text": "irrelevant here"},
        ],
    }

    # Same shape, but the story-1 alternate's page is about something else —
    # the content-word gate must still refuse it even though the story
    # number matches.
    POOL_WITH_UNRELATED_ALTERNATE = {
        "publication": "ai-pulse", "date": "2026-07-30",
        "candidates": [
            {"id": 4, "story": 1, "title": "Anthropic hack - Reuters",
             "url": ANTHROPIC_URL, "source": "reuters.com",
             "published": "2026-07-29", "result_id": 100, "text": "irrelevant here"},
            {"id": 15, "story": 1, "title": "Unrelated coverage",
             "url": ALTERNATE_URL, "source": "apnews.com", "published": "2026-07-29",
             "result_id": 101,
             "text": "This paragraph is about something else entirely and does not "
                     "overlap with the post's claim in any meaningful way today."},
            {"id": 9, "story": 2, "title": "OpenAI price cuts - PYMNTS",
             "url": OPENAI_URL, "source": "pymnts.com",
             "published": "2026-07-29", "result_id": 102, "text": "irrelevant here"},
        ],
    }

    POOL_WITH_NO_ALTERNATE = {
        "publication": "ai-pulse", "date": "2026-07-30",
        "candidates": [
            {"id": 4, "story": 1, "title": "Anthropic hack - Reuters",
             "url": ANTHROPIC_URL, "source": "reuters.com",
             "published": "2026-07-29", "result_id": 100, "text": "irrelevant here"},
            {"id": 9, "story": 2, "title": "OpenAI price cuts - PYMNTS",
             "url": OPENAI_URL, "source": "pymnts.com",
             "published": "2026-07-29", "result_id": 102, "text": "irrelevant here"},
        ],
    }

    def _publish(self, pool, issue=None, broken=None):
        broken = broken if broken is not None else {self.ANTHROPIC_URL}
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, issue=issue, pool=pool)
            with mock.patch.object(pi, "check_link", side_effect=_link_map(broken)):
                with mock.patch.object(pi.subprocess, "run") as proc:
                    proc.return_value = mock.Mock(
                        returncode=0, stdout="  ✓ Sent to a@b.com\n", stderr="")
                    with redirect_stdout(io.StringIO()) as out:
                        code = pi.publish(DAY, "ai-pulse", f.dir, send=True)
            posts_path = f.dir / "X_posts_2026-07-30.txt"
            posts = posts_path.read_text(encoding="utf-8") if posts_path.exists() else None
            return code, out.getvalue(), f.record(), posts

    def test_a_broken_link_is_repaired_from_a_same_story_candidate(self):
        code, out, record, posts = self._publish(self.POOL_WITH_ALTERNATE)
        self.assertEqual(code, 0)
        self.assertEqual(record["status"], "sent")
        self.assertEqual(record["selected"], 2)
        self.assertEqual(record["links"], {"ok": 2, "suspect": 0, "broken": 0})
        self.assertNotIn("dropped", record)
        self.assertEqual(len(record["repaired"]), 1)
        self.assertEqual(record["repaired"][0]["new_url"], self.ALTERNATE_URL)
        self.assertIn(self.ALTERNATE_URL, posts)
        self.assertNotIn(self.ANTHROPIC_URL, posts)
        self.assertIn("REPAIRED", out)

    def test_a_same_story_alternate_that_does_not_support_the_post_is_refused(self):
        # The story number matching is not enough on its own — the content-
        # word gate build_issue used at selection time applies again here,
        # or a repair could silently attach a post to the wrong page.
        code, out, record, posts = self._publish(self.POOL_WITH_UNRELATED_ALTERNATE)
        self.assertEqual(code, 0)                    # 1 of 2 items still ships
        self.assertEqual(record["selected"], 1)
        self.assertNotIn("repaired", record)
        self.assertEqual(len(record["dropped"]), 1)
        self.assertNotIn(self.ALTERNATE_URL, posts)

    def test_a_broken_link_with_no_alternate_is_dropped_and_the_rest_still_sends(self):
        code, out, record, posts = self._publish(self.POOL_WITH_NO_ALTERNATE)
        self.assertEqual(code, 0)
        self.assertEqual(record["status"], "sent")
        self.assertEqual(record["selected"], 1)
        self.assertEqual(len(record["dropped"]), 1)
        self.assertNotIn("repaired", record)
        # Renumbered: the sole survivor is #1, not #2 — post_tweet.py indexes
        # a fixed daily schedule by position, and a gap would post nothing
        # for one slot and the wrong item for every slot after it.
        self.assertIn("1. 📉 OpenAI cuts prices on select models.", posts)
        self.assertNotIn("Anthropic", posts)

    def test_dropping_below_min_items_still_blocks(self):
        issue = dict(ISSUE, min_items=2)
        code, out, record, _ = self._publish(self.POOL_WITH_NO_ALTERNATE, issue=issue)
        self.assertEqual(code, 2)
        self.assertEqual(record["status"], "blocked")
        self.assertEqual(len(record["dropped"]), 1)
        self.assertIn("need", record["reason"].lower())
        self.assertIn("Nothing was sent", out)


class PublicationConfig(unittest.TestCase):
    """The publication's settings arrive inside the issue JSON rather than being
    parsed out of YAML here — one config dialect, in one language, in one place.
    These tests are what makes "a second publication is a config file" true on
    the Python side of the boundary."""

    def test_defaults_when_the_issue_carries_no_config(self):
        # A publication with no config still produces exactly what the AI
        # newsletter always produced.
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp)
            f.run()
            self.assertTrue((f.dir / "X_posts_2026-07-30.txt").exists())
            self.assertTrue((f.dir / "newsletter_2026-07-30.html").exists())
            self.assertEqual(f.record()["artifacts"],
                             ["posts_txt", "newsletter_html"])

    def test_configured_artifact_paths_are_honoured(self):
        issue = dict(ISSUE, artifacts=[
            {"kind": "newsletter_html", "path": "{workspace}/out/brief_{iso_date}.html",
             "template": "newsletter_template.html"},
        ])
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, issue)
            code, _ = f.run()
            self.assertEqual(code, 0)
            # Only what was asked for, at the path that was asked for, in a
            # directory that did not exist.
            self.assertTrue((f.dir / "out" / "brief_2026-07-30.html").exists())
            self.assertFalse((f.dir / "X_posts_2026-07-30.txt").exists())
            self.assertEqual(f.record()["artifacts"], ["newsletter_html"])

    def test_an_unknown_artifact_kind_is_refused(self):
        # Quietly skipping it would ship an issue missing half of itself.
        issue = dict(ISSUE, artifacts=[{"kind": "podcast", "path": "{workspace}/x"}])
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, issue)
            code, _ = f.run()
            self.assertEqual(code, 1)
            self.assertIn("unknown artifact kind 'podcast'", f.record()["reason"])

    def test_the_configured_subject_reaches_the_sender(self):
        issue = dict(ISSUE, subject="Security Brief — {iso_date}")
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, issue)
            with mock.patch.object(pi, "check_link", return_value=("ok", "200")):
                with mock.patch.object(pi.subprocess, "run") as proc:
                    proc.return_value = mock.Mock(returncode=0, stdout="", stderr="")
                    with redirect_stdout(io.StringIO()):
                        pi.publish(DAY, "ai-pulse", f.dir, send=True)
            cmd = proc.call_args.args[0]
        self.assertEqual(cmd[cmd.index("--subject") + 1], "Security Brief — 2026-07-30")

    def test_the_configured_recipients_file_reaches_the_sender(self):
        issue = dict(ISSUE, channels=[{"kind": "email",
                                       "recipients_file": "defenders.txt"}])
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, issue)
            with mock.patch.object(pi, "check_link", return_value=("ok", "200")):
                with mock.patch.object(pi.subprocess, "run") as proc:
                    proc.return_value = mock.Mock(returncode=0, stdout="", stderr="")
                    with redirect_stdout(io.StringIO()):
                        pi.publish(DAY, "ai-pulse", f.dir, send=True)
            cmd = proc.call_args.args[0]
        self.assertEqual(cmd[cmd.index("--recipients") + 1], "defenders.txt")

    def test_a_publication_with_no_email_channel_renders_and_stops(self):
        # An explicitly empty channel list is a legitimate configuration, not a
        # failure — and it must not be confused with having no channels key.
        issue = dict(ISSUE, channels=[])
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, issue)
            with mock.patch.object(pi, "check_link", return_value=("ok", "200")):
                with mock.patch.object(pi.subprocess, "run") as proc:
                    with redirect_stdout(io.StringIO()) as out:
                        code = pi.publish(DAY, "ai-pulse", f.dir, send=True)
            record = f.record()
            # It still rendered — for such a publication the artifacts are the
            # whole point.
            rendered = (f.dir / "newsletter_2026-07-30.html").exists()
        self.assertEqual(code, 0)
        proc.assert_not_called()
        self.assertEqual(record["status"], "rendered")
        self.assertEqual(record["reason"], "no email channel")
        self.assertIn("nothing to send", out.getvalue())
        self.assertTrue(rendered)

    def test_substitute_covers_both_date_forms(self):
        self.assertEqual(pi.substitute("{iso_date}", Path("/w"), DAY), "2026-07-30")
        self.assertEqual(pi.substitute("{date}", Path("/w"), DAY),
                         "Thursday, July 30, 2026")
        self.assertEqual(pi.substitute("{workspace}/x", Path("/w"), DAY), "/w/x")


class BadIssues(unittest.TestCase):
    def _publish(self, issue):
        with TemporaryDirectory() as tmp:
            f = Fixture(tmp, issue)
            code, out = f.run()
            return code, out, f.record()

    def test_an_item_without_a_url_is_refused(self):
        broken = json.loads(json.dumps(ISSUE))
        del broken["items"][1]["url"]
        code, _, record = self._publish(broken)
        self.assertEqual(code, 1)
        self.assertIn("item 2 has no url", record["reason"])

    def test_an_issue_with_no_items_is_refused(self):
        empty = dict(ISSUE, items=[])
        code, _, record = self._publish(empty)
        self.assertEqual(code, 1)
        self.assertIn("no items", record["reason"])


if __name__ == "__main__":
    unittest.main()
