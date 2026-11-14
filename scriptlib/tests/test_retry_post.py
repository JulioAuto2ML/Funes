"""Unit tests for retry_post (7.23): what it accepts, and that it runs
post_tweet.py for exactly one slot of one publication and logs what it said.

post_tweet.py is replaced by a fake that records its argv — the real one talks
to LinkedIn, and its own refusal of an already-posted slot is tested in
publishing/tests.
"""

import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import retry_post  # noqa: E402

FAKE = """import json, sys
open(sys.argv[0] + ".calls", "a").write(json.dumps(sys.argv[1:]) + "\\n")
print("Posted successfully! Post ID: urn:li:share:1")
sys.exit(int(__import__("os").environ.get("FAKE_EXIT", "0")))
"""


class RetryPost(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "publishing").mkdir()
        self.fake = self.root / "publishing" / "post_tweet.py"
        self.fake.write_text(FAKE)
        self.news = self.root / "data" / "newsletter"
        self.news.mkdir(parents=True)
        self.env = {"FUNES_ROOT": str(self.root), "NEWSLETTER_DIR": str(self.news)}

    def tearDown(self):
        self.tmp.cleanup()

    def run_it(self, argv, extra=None):
        out = io.StringIO()
        with mock.patch.dict(os.environ, {**self.env, **(extra or {})}), redirect_stdout(out):
            rc = retry_post.main(argv)
        return rc, json.loads(out.getvalue())

    def calls(self):
        path = Path(str(self.fake) + ".calls")
        return [json.loads(l) for l in path.read_text().splitlines()] if path.exists() else []

    def test_runs_one_slot_and_logs_it(self):
        rc, out = self.run_it(["--post=ai-pulse-es.5"])
        self.assertEqual(rc, 0)
        self.assertTrue(out["ok"])
        self.assertEqual(self.calls(), [["5", "--dir", str(self.news), "--publication", "ai-pulse-es"]])
        log = (self.news / "post_log.txt").read_text()
        self.assertIn("[retry_post ai-pulse-es.5]", log)
        self.assertIn("Posted successfully", log)

    def test_slash_form(self):
        self.run_it(["--post=ai-pulse/2"])
        self.assertEqual(self.calls()[0][:1], ["2"])

    def test_failure_is_reported(self):
        rc, out = self.run_it(["--post=ai-pulse.1"], {"FAKE_EXIT": "3"})
        self.assertEqual(rc, 1)
        self.assertFalse(out["ok"])
        self.assertEqual(out["exit_code"], 3)

    def test_refuses_anything_else(self):
        for bad in ["", "ai-pulse", "ai-pulse/x", "../etc/1", "ai pulse/1", "ai-pulse/1;rm", "AI/1"]:
            with self.subTest(bad=bad), self.assertRaises(SystemExit):
                self.run_it([f"--post={bad}"])
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main()
