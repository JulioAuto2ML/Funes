"""Unit tests for publish_briefing.py.

What is under test is what the script lets through to Alexa — text that will
be read aloud as written, so anything that reads badly (a URL, markdown, a
fragment) is refused with a reason the model can act on — and that the feed
file is written whole or not at all.
"""

import io
import json
import os
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime, timezone
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import publish_briefing as pb  # noqa: E402

NOW = datetime(2026, 10, 23, 10, 30, tzinfo=timezone.utc)
GOOD = ("Buen día. Estas son las noticias de hoy. " * 12).strip()


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        env = mock.patch.dict(os.environ, {
            "FUNES_NEWS_FEED_DIR": self.tmp.name,
            "FUNES_NEWS_PUBLIC_URL": "https://yoda.example.ts.net/noticias/feed.json",
        })
        env.start()
        self.addCleanup(env.stop)
        now = mock.patch.object(pb, "_now", return_value=NOW)
        now.start()
        self.addCleanup(now.stop)

    def run_pb(self, *argv):
        with redirect_stderr(io.StringIO()) as err:
            code, result = pb.perform(list(argv))
        self.stderr = err.getvalue()
        return code, result

    @property
    def feed_path(self):
        return Path(self.tmp.name) / "feed.json"


class TestText(Base):
    def test_too_short_is_refused(self):
        self.assertEqual(self.run_pb("--text=Hola.")[0], 2)
        self.assertFalse(self.feed_path.exists())

    def test_too_long_is_refused_not_truncated(self):
        code, _ = self.run_pb("--text=" + "Una frase más. " * 400)
        self.assertEqual(code, 2)
        self.assertIn(str(pb.MAX_CHARS), self.stderr)

    def test_urls_are_refused(self):
        for bad in ("Más en https://infobae.com hoy.", "Visite www.clarin.com para más."):
            code, _ = self.run_pb(f"--text={GOOD} {bad}")
            self.assertEqual(code, 2, bad)
            self.assertIn("URL", self.stderr)

    def test_markdown_is_refused(self):
        for bad in ("**Política**: el gobierno", "## Economía", "- primer punto", "[link](x)", "`code`"):
            code, _ = self.run_pb(f"--text={GOOD}\n{bad} sigue el texto.")
            self.assertEqual(code, 2, bad)

    def test_tags_are_refused(self):
        code, _ = self.run_pb(f"--text={GOOD} <speak>hola</speak>")
        self.assertEqual(code, 2)

    def test_ordinary_punctuation_is_fine(self):
        text = GOOD + " El dólar cerró a 1.250 pesos (un 2% más); la CGT para el jueves: «sin cambios»."
        self.assertEqual(self.run_pb(f"--text={text}")[0], 0)

    def test_text_must_end_a_sentence(self):
        code, _ = self.run_pb(f"--text={GOOD} y entonces el")
        self.assertEqual(code, 2)


class TestFeed(Base):
    def test_feed_has_the_fields_alexa_requires(self):
        code, result = self.run_pb(f"--text={GOOD}")
        self.assertEqual(code, 0)
        feed = json.loads(self.feed_path.read_text(encoding="utf-8"))
        self.assertEqual(len(feed), 1)
        entry = feed[0]
        self.assertEqual(set(entry), {"uid", "updateDate", "titleText", "mainText", "redirectionUrl"})
        self.assertEqual(entry["updateDate"], "2026-10-23T10:30:00.0Z")
        self.assertEqual(entry["mainText"], GOOD)
        self.assertTrue(entry["uid"].startswith("urn:uuid:"))
        self.assertEqual(entry["redirectionUrl"], "https://yoda.example.ts.net/noticias/feed.json")

    def test_default_title_names_the_date(self):
        self.run_pb(f"--text={GOOD}")
        entry = json.loads(self.feed_path.read_text(encoding="utf-8"))[0]
        self.assertIn("23", entry["titleText"])

    def test_each_publish_replaces_the_single_item(self):
        self.run_pb(f"--text={GOOD}")
        first = json.loads(self.feed_path.read_text(encoding="utf-8"))[0]["uid"]
        self.run_pb(f"--text={GOOD} Una más.")
        feed = json.loads(self.feed_path.read_text(encoding="utf-8"))
        self.assertEqual(len(feed), 1)
        self.assertNotEqual(feed[0]["uid"], first)

    def test_a_failed_write_leaves_the_previous_feed_intact(self):
        self.run_pb(f"--text={GOOD}")
        before = self.feed_path.read_text(encoding="utf-8")
        with mock.patch.object(pb.os, "replace", side_effect=OSError("disk full")):
            code, _ = self.run_pb(f"--text={GOOD} Otra versión.")
        self.assertEqual(code, 1)
        self.assertEqual(self.feed_path.read_text(encoding="utf-8"), before)
        self.assertEqual([p.name for p in Path(self.tmp.name).iterdir()], ["feed.json"])

    def test_result_reports_length_and_spoken_time(self):
        code, result = self.run_pb(f"--text={GOOD}")
        self.assertEqual(result["chars"], len(GOOD))
        self.assertGreater(result["est_seconds"], 0)


class TestConfig(Base):
    def test_missing_dir_is_an_installation_fault(self):
        with mock.patch.dict(os.environ, {"FUNES_NEWS_FEED_DIR": ""}):
            self.assertEqual(self.run_pb(f"--text={GOOD}")[0], 3)

    def test_relative_dir_is_refused(self):
        with mock.patch.dict(os.environ, {"FUNES_NEWS_FEED_DIR": "relative/dir"}):
            self.assertEqual(self.run_pb(f"--text={GOOD}")[0], 3)

    def test_nonexistent_dir_is_refused_not_created(self):
        missing = os.path.join(self.tmp.name, "nope")
        with mock.patch.dict(os.environ, {"FUNES_NEWS_FEED_DIR": missing}):
            self.assertEqual(self.run_pb(f"--text={GOOD}")[0], 3)
        self.assertFalse(os.path.exists(missing))

    def test_missing_public_url_is_an_installation_fault(self):
        with mock.patch.dict(os.environ, {"FUNES_NEWS_PUBLIC_URL": ""}):
            self.assertEqual(self.run_pb(f"--text={GOOD}")[0], 3)


class TestMain(Base):
    def test_stdout_is_one_json_object(self):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(io.StringIO()):
            self.assertEqual(pb.main([f"--text={GOOD}"]), 0)
        self.assertTrue(json.loads(out.getvalue())["ok"])


if __name__ == "__main__":
    unittest.main()
