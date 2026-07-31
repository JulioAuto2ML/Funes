"""Unit tests for send_newsletter.py.

SMTP itself is not tested — that is smtplib's job, and a test that opens a real
connection to Gmail is not a test. What is tested is everything around it: where
the credentials and the mailing list are read from, what the subject line says,
and that --dry-run really sends nothing. The last one matters because --dry-run
is what a human reaches for before a first live run.
"""

import io
import unittest
from contextlib import redirect_stdout
from datetime import date
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

import send_newsletter as sn


class LoadEnv(unittest.TestCase):
    def test_missing_file_is_empty_not_an_error(self):
        with TemporaryDirectory() as tmp:
            self.assertEqual(sn.load_env(Path(tmp) / ".env"), {})

    def test_comments_and_blanks_are_skipped(self):
        with TemporaryDirectory() as tmp:
            env = Path(tmp) / ".env"
            env.write_text("# a comment\n\nGMAIL_ADDRESS = a@b.com \nX=1=2\n")
            loaded = sn.load_env(env)
        self.assertEqual(loaded["GMAIL_ADDRESS"], "a@b.com")
        self.assertEqual(loaded["X"], "1=2")          # only the first = splits
        self.assertNotIn("# a comment", loaded)


class LoadSubscribers(unittest.TestCase):
    def test_missing_file_warns_and_returns_empty(self):
        with TemporaryDirectory() as tmp:
            with redirect_stdout(io.StringIO()) as out:
                addrs = sn.load_subscribers(Path(tmp) / "subscribers.txt")
        self.assertEqual(addrs, [])
        self.assertIn("WARNING", out.getvalue())

    def test_comments_and_blanks_are_skipped(self):
        with TemporaryDirectory() as tmp:
            f = Path(tmp) / "subscribers.txt"
            f.write_text("# people\na@b.com\n\n  c@d.com  \n")
            self.assertEqual(sn.load_subscribers(f), ["a@b.com", "c@d.com"])


class LoadNewsletter(unittest.TestCase):
    def test_reads_the_issue_for_that_date(self):
        with TemporaryDirectory() as tmp:
            (Path(tmp) / "newsletter_2026-07-30.html").write_text("<p>hi</p>")
            subject, body = sn.load_newsletter(Path(tmp), date(2026, 7, 30))
        self.assertEqual(body, "<p>hi</p>")
        self.assertEqual(subject, "AI Pulse · Thursday, July 30, 2026")

    def test_subject_day_is_not_zero_padded(self):
        with TemporaryDirectory() as tmp:
            (Path(tmp) / "newsletter_2026-07-03.html").write_text("x")
            subject, _ = sn.load_newsletter(Path(tmp), date(2026, 7, 3))
        self.assertIn("July 3, 2026", subject)

    def test_a_missing_issue_raises(self):
        with TemporaryDirectory() as tmp:
            with self.assertRaises(FileNotFoundError):
                sn.load_newsletter(Path(tmp), date(2026, 7, 30))


class SendNewsletter(unittest.TestCase):
    HTML = "<html><body><p>Hello &amp; welcome</p></body></html>"

    def _send(self, subscribers, dry_run):
        server = mock.MagicMock()
        smtp = mock.MagicMock()
        smtp.return_value.__enter__.return_value = server
        with mock.patch.object(sn.smtplib, "SMTP", smtp):
            with redirect_stdout(io.StringIO()) as out:
                sn.send_newsletter("me@gmail.com", "pw", subscribers,
                                   "Subject", self.HTML, dry_run=dry_run)
        return server, out.getvalue()

    def test_no_subscribers_opens_no_connection(self):
        server, out = self._send([], dry_run=False)
        server.login.assert_not_called()
        self.assertIn("nothing to send", out)

    def test_one_message_per_subscriber(self):
        server, _ = self._send(["a@b.com", "c@d.com"], dry_run=False)
        self.assertEqual(server.sendmail.call_count, 2)

    def test_dry_run_sends_nothing(self):
        server, out = self._send(["a@b.com"], dry_run=True)
        server.sendmail.assert_not_called()
        self.assertIn("DRY RUN", out)

    def test_message_carries_a_plain_text_alternative(self):
        server, _ = self._send(["a@b.com"], dry_run=False)
        raw = server.sendmail.call_args.args[2]
        self.assertIn("text/plain", raw)
        self.assertIn("text/html", raw)


if __name__ == "__main__":
    unittest.main()
