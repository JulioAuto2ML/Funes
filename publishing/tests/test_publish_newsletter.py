"""Unit tests for publish_newsletter.py.

These grew out of the script's old inline --self-test. That version could only
cover the pure functions, because everything else reached the network or the
filesystem relative to the script's own directory. Now that the issue directory
is a parameter, the interesting parts — the exit codes, and the guarantee that
--send is unreachable with a broken link — are testable too, and they are the
parts a caller actually branches on.

check_link is exercised against a stubbed urlopen rather than the real web: the
distinction it draws between "broken" and "suspect" is a policy decision about
status codes, and a test that needs a 429 from a real server cannot be written.
"""

import io
import sys
import unittest
import urllib.error
from contextlib import redirect_stdout
from datetime import date
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

import publish_newsletter as pn


SAMPLE = (
    "X Posts — July 30, 2026\n"
    "Generated from trending AI/LLM/AGI topics\n"
    "============================================================\n"
    "\n"
    "1. \U0001f9ec First post text.\n"
    "https://www.engadget.com/2225849/google-shuts-down-alphafold/\n"
    "\n"
    "---\n"
    "\n"
    "2. \U0001f6d1 Second post, whose text\n"
    "wrapped onto another line.\n"
    "https://example.com/a/b\n"
)


class ParsePosts(unittest.TestCase):
    def test_parses_every_numbered_block(self):
        posts = pn.parse_posts(SAMPLE)
        self.assertEqual(len(posts), 2)
        self.assertEqual(posts[0]["n"], 1)          # header block skipped
        self.assertIn("\U0001f9ec", posts[0]["text"])
        self.assertEqual(posts[1]["url"], "https://example.com/a/b")

    def test_joins_text_wrapped_over_lines(self):
        self.assertTrue(pn.parse_posts(SAMPLE)[1]["text"].endswith("another line."))

    def test_separators_are_optional(self):
        posts = pn.parse_posts("1. one\nhttps://a.com\n2. two\nhttps://b.com\n")
        self.assertEqual(len(posts), 2)

    def test_headline_after_url_in_any_dash(self):
        posts = pn.parse_posts(
            "1. one\nhttps://www.engadget.com/x — Google Disbands AlphaFold\n"
            "2. two\nhttps://b.com/y | Pipe Style\n"
            "3. three\nhttps://c.com/z\n")
        self.assertEqual(posts[0]["headline"], "Google Disbands AlphaFold")
        self.assertEqual(posts[1]["headline"], "Pipe Style")
        self.assertIsNone(posts[2]["headline"])
        # The headline must not be glued onto the URL it follows.
        self.assertTrue(posts[0]["url"].endswith("/x"))

    def test_hyphenated_slug_is_not_a_headline(self):
        posts = pn.parse_posts("1. x\nhttps://a.com/some-long-slug-here\n")
        self.assertIsNone(posts[0]["headline"])

    def test_post_without_url_raises_naming_it(self):
        # Silently dropping it would hide exactly what the link check exists for.
        with self.assertRaises(ValueError) as cm:
            pn.parse_posts("1. orphan post with no link\n")
        self.assertIn("1", str(cm.exception))

    def test_prose_with_no_posts_raises(self):
        with self.assertRaises(ValueError):
            pn.parse_posts("just some prose\n")


class LinkLabel(unittest.TestCase):
    def test_headline_wins(self):
        post = {"url": "https://www.engadget.com/x", "headline": "Big News"}
        self.assertEqual(pn.link_label(post), "engadget.com — Big News")

    def test_falls_back_to_bare_host(self):
        self.assertEqual(pn.link_label({"url": "https://c.com/z"}), "c.com")


class RenderHtml(unittest.TestCase):
    NASTY = [{"n": 1, "text": 'Ampersand & <script>alert("x")</script>',
              "url": "https://www.example.com/a?x=1&y=2"}]

    def test_weekday_is_computed_not_carried_over(self):
        # The 2026-07-30 issue went out labelled "Wednesday" because the older
        # LLM-driven pipeline copied yesterday's weekday forward. It was a
        # Thursday.
        out = pn.render_html("<p>{{DATE}}</p>\n      {{POSTS}}\n",
                             date(2026, 7, 30), self.NASTY)
        self.assertIn("Thursday, July 30, 2026", out)

    def test_day_is_not_zero_padded(self):
        out = pn.render_html("{{DATE}}{{POSTS}}", date(2026, 7, 3), self.NASTY)
        self.assertNotIn("July 03", out)

    def test_escapes_text_and_href(self):
        out = pn.render_html("{{DATE}}{{POSTS}}", date(2026, 7, 30), self.NASTY)
        self.assertNotIn("<script>", out)
        self.assertIn("Ampersand &amp;", out)
        self.assertIn("x=1&amp;y=2", out)

    def test_leaves_no_placeholder_behind(self):
        out = pn.render_html("{{DATE}}{{POSTS}}", date(2026, 7, 30), self.NASTY)
        self.assertNotIn("{{POSTS}}", out)
        self.assertNotIn("{{DATE}}", out)

    def test_items_share_the_placeholder_indent(self):
        two = [dict(self.NASTY[0], n=1), dict(self.NASTY[0], n=2)]
        out = pn.render_html("<p>{{DATE}}</p>\n      {{POSTS}}\n",
                             date(2026, 7, 30), two)
        item_lines = [l for l in out.splitlines() if '<div class="post-item">' in l]
        self.assertEqual(len(item_lines), 2)
        self.assertEqual(len(set(item_lines)), 1)         # same indentation
        self.assertTrue(item_lines[0].startswith('      <div'))

    def test_shipped_template_still_has_both_placeholders(self):
        tpl = (Path(pn.SCRIPT_DIR) / pn.TEMPLATE_NAME).read_text(encoding="utf-8")
        self.assertIn("{{DATE}}", tpl)
        self.assertIn("{{POSTS}}", tpl)


def _http_error(code):
    return urllib.error.HTTPError("https://x.test/", code, "nope", {}, None)


class _Response:
    def __init__(self, status):
        self.status = status

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class CheckLink(unittest.TestCase):
    """`outcomes` maps request method to what urlopen should do for it — or to a
    list, consumed one entry per call, for testing the retry."""

    def setUp(self):
        # The retry pause is real time; nothing here is testing that it sleeps.
        patcher = mock.patch.object(pn.time, "sleep")
        self.slept = patcher.start()
        self.addCleanup(patcher.stop)

    def _stub(self, outcomes):
        seen = []
        pending = {k: list(v) if isinstance(v, list) else None
                   for k, v in outcomes.items()}

        def fake_urlopen(req, timeout=None):
            method = req.get_method()
            seen.append(method)
            queue = pending[method]
            result = queue.pop(0) if queue else outcomes[method]
            if isinstance(result, Exception):
                raise result
            return _Response(result)

        return mock.patch.object(pn.urllib.request, "urlopen", fake_urlopen), seen

    def test_head_200_is_ok(self):
        patcher, seen = self._stub({"HEAD": 200})
        with patcher:
            self.assertEqual(pn.check_link("https://x.test/")[0], "ok")
        self.assertEqual(seen, ["HEAD"])                  # no needless GET

    def test_head_405_retries_as_get(self):
        # Plenty of servers refuse HEAD and serve GET fine.
        patcher, seen = self._stub({"HEAD": _http_error(405), "GET": 200})
        with patcher:
            self.assertEqual(pn.check_link("https://x.test/")[0], "ok")
        self.assertEqual(seen, ["HEAD", "GET"])

    def test_404_is_broken(self):
        patcher, _ = self._stub({"HEAD": _http_error(404)})
        with patcher:
            status, detail = pn.check_link("https://x.test/")
        self.assertEqual(status, "broken")
        self.assertIn("404", detail)

    def test_429_is_suspect_not_broken(self):
        # Bot-blocking and rate limiting must never stall an issue.
        patcher, _ = self._stub({"HEAD": _http_error(429)})
        with patcher:
            self.assertEqual(pn.check_link("https://x.test/")[0], "suspect")

    def test_403_after_the_get_retry_is_suspect(self):
        patcher, seen = self._stub({"HEAD": _http_error(403), "GET": _http_error(403)})
        with patcher:
            self.assertEqual(pn.check_link("https://x.test/")[0], "suspect")
        self.assertEqual(seen, ["HEAD", "GET"])

    def test_connection_error_on_both_methods_is_broken(self):
        err = urllib.error.URLError("no route to host")
        patcher, seen = self._stub({"HEAD": err, "GET": err})
        with patcher:
            status, detail = pn.check_link("https://x.test/", retries=0)
        self.assertEqual(status, "broken")
        self.assertIn("URLError", detail)
        self.assertEqual(seen, ["HEAD", "GET"])

    def test_a_timeout_is_retried_once(self):
        # washingtonpost.com was fetched fine by the harvester and timed out on
        # this check a minute later, which refused the whole issue.
        patcher, seen = self._stub({"HEAD": [TimeoutError("timed out"), 200],
                                    "GET": TimeoutError("timed out")})
        with patcher:
            status, _ = pn.check_link("https://x.test/")
        self.assertEqual(status, "ok")
        self.assertEqual(seen, ["HEAD", "GET", "HEAD"])
        self.assertEqual(self.slept.call_count, 1)

    def test_a_persistent_timeout_is_still_broken(self):
        err = TimeoutError("timed out")
        patcher, seen = self._stub({"HEAD": err, "GET": err})
        with patcher:
            status, _ = pn.check_link("https://x.test/")
        self.assertEqual(status, "broken")
        self.assertEqual(seen, ["HEAD", "GET", "HEAD", "GET"])   # tried twice

    def test_a_persistent_timeout_hints_at_a_domain_block(self):
        # The repair loop only gets one signal to tell "page is gone" apart
        # from "this host can't reach the domain at all" — without it, on
        # 2026-08-04 it spent both repair attempts on two more forbes.com
        # URLs for the same story that were never going to work either.
        err = TimeoutError("timed out")
        patcher, _ = self._stub({"HEAD": err, "GET": err})
        with patcher:
            _, detail = pn.check_link("https://x.test/")
        self.assertIn("DIFFERENT domain", detail)

    def test_an_http_404_does_not_hint_at_a_domain_block(self):
        # The server answered on purpose here — the domain is fine, only the
        # page is gone, so another URL on the same site is a reasonable fix.
        patcher, _ = self._stub({"HEAD": _http_error(404)})
        with patcher:
            _, detail = pn.check_link("https://x.test/")
        self.assertNotIn("DIFFERENT domain", detail)

    def test_an_http_status_is_never_retried(self):
        # The server answered. Asking again will not change its mind, and ten
        # pointless retries would double the length of every run.
        patcher, seen = self._stub({"HEAD": _http_error(404)})
        with patcher:
            self.assertEqual(pn.check_link("https://x.test/")[0], "broken")
        self.assertEqual(seen, ["HEAD"])
        self.slept.assert_not_called()

    def test_a_malformed_url_is_never_retried(self):
        patcher, seen = self._stub({"HEAD": ValueError("unknown url type")})
        with patcher:
            self.assertEqual(pn.check_link("gopher://x.test/")[0], "broken")
        self.assertEqual(seen, ["HEAD"])


class IssueDirResolution(unittest.TestCase):
    def test_explicit_dir_wins_over_env(self):
        with TemporaryDirectory() as tmp:
            with mock.patch.dict(pn.os.environ, {"FUNES_PUBLISH_DIR": "/nowhere"}):
                self.assertEqual(pn.issue_dir(tmp), Path(tmp).resolve())

    def test_env_is_used_when_no_flag(self):
        with TemporaryDirectory() as tmp:
            with mock.patch.dict(pn.os.environ, {"FUNES_PUBLISH_DIR": tmp}):
                self.assertEqual(pn.issue_dir(), Path(tmp).resolve())

    def test_template_falls_back_to_the_repo_copy(self):
        with TemporaryDirectory() as tmp:
            self.assertEqual(pn.template_path(Path(tmp)),
                             Path(pn.SCRIPT_DIR) / pn.TEMPLATE_NAME)

    def test_issue_dir_template_overrides_the_repo_copy(self):
        with TemporaryDirectory() as tmp:
            local = Path(tmp) / pn.TEMPLATE_NAME
            local.write_text("{{DATE}}{{POSTS}}")
            self.assertEqual(pn.template_path(Path(tmp)), local)


class PublishExitCodes(unittest.TestCase):
    """The exit code is the whole interface a caller — human, agent or cron —
    branches on, so each one gets a test."""

    def _issue(self, tmp, body=None):
        work = Path(tmp)
        (work / pn.TEMPLATE_NAME).write_text("<p>{{DATE}}</p>\n  {{POSTS}}\n")
        (work / "X_posts_2026-07-30.txt").write_text(
            body if body is not None else "1. one\nhttps://a.test/x\n")
        return work

    def _publish(self, work, send=False, link_status=("ok", "200")):
        with mock.patch.object(pn, "check_link", return_value=link_status):
            with redirect_stdout(io.StringIO()) as out:
                code = pn.publish(date(2026, 7, 30), send, work_dir=work)
        return code, out.getvalue()

    def test_missing_posts_file_is_1(self):
        with TemporaryDirectory() as tmp:
            code, out = self._publish(Path(tmp))
        self.assertEqual(code, 1)
        self.assertIn("posts file not found", out)

    def test_unparseable_posts_file_is_1(self):
        with TemporaryDirectory() as tmp:
            work = self._issue(tmp, "1. a post with no link at all\n")
            code, out = self._publish(work)
        self.assertEqual(code, 1)
        self.assertIn("could not parse", out)

    def test_broken_link_is_2_and_sends_nothing(self):
        with TemporaryDirectory() as tmp:
            work = self._issue(tmp)
            with mock.patch.object(pn.subprocess, "run") as run:
                code, out = self._publish(work, send=True,
                                          link_status=("broken", "HTTP 404"))
        self.assertEqual(code, 2)
        self.assertIn("Nothing was sent", out)
        run.assert_not_called()

    def test_suspect_link_does_not_block(self):
        with TemporaryDirectory() as tmp:
            work = self._issue(tmp)
            code, out = self._publish(work, link_status=("suspect", "HTTP 403"))
        self.assertEqual(code, 0)
        self.assertIn("SUSPECT", out)

    def test_render_only_is_0_and_writes_the_html(self):
        with TemporaryDirectory() as tmp:
            work = self._issue(tmp)
            code, out = self._publish(work)
        self.assertEqual(code, 0)
        self.assertIn("Nothing was sent", out)

    def test_html_is_written_beside_the_posts_file(self):
        with TemporaryDirectory() as tmp:
            work = self._issue(tmp)
            self._publish(work)
            rendered = work / "newsletter_2026-07-30.html"
            self.assertTrue(rendered.exists())
            self.assertIn("https://a.test/x", rendered.read_text())

    def test_send_passes_the_issue_dir_to_the_sender(self):
        # The sender lives in the repo but must read .env, subscribers.txt and
        # the rendered HTML from the issue directory, not from beside itself.
        with TemporaryDirectory() as tmp:
            work = self._issue(tmp)
            with mock.patch.object(pn.subprocess, "run") as run:
                run.return_value = mock.Mock(returncode=0, stdout="ok", stderr="")
                code, _ = self._publish(work, send=True)
            cmd = run.call_args.args[0]
        self.assertEqual(code, 0)
        self.assertIn("--dir", cmd)
        self.assertEqual(cmd[cmd.index("--dir") + 1], str(work))
        self.assertTrue(cmd[1].endswith("send_newsletter.py"))

    def test_a_failing_send_is_reported_not_swallowed(self):
        with TemporaryDirectory() as tmp:
            work = self._issue(tmp)
            with mock.patch.object(pn.subprocess, "run") as run:
                run.return_value = mock.Mock(returncode=3, stdout="", stderr="smtp died")
                code, out = self._publish(work, send=True)
        self.assertEqual(code, 3)
        self.assertIn("NOT sent", out)


if __name__ == "__main__":
    unittest.main()
