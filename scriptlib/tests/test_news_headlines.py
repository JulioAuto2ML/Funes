"""Unit tests for news_headlines.py.

The network is stubbed through `news_headlines._fetch`: what is under test is
the script's own editorial decisions — which items are recent enough, which
items from different outlets are the same story, and that a story several
outlets cover outranks one only one outlet has. Whether the real feeds still
answer is checked by running the script, not here.
"""

import io
import json
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime, timedelta, timezone
from email.utils import format_datetime
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import news_headlines as nh  # noqa: E402

NOW = datetime(2026, 10, 23, 12, 0, tzinfo=timezone.utc)


def rss(*items):
    body = "".join(
        f"<item><title>{t}</title><description><![CDATA[{d}]]></description>"
        f"<pubDate>{format_datetime(NOW - timedelta(hours=h))}</pubDate>"
        + (f"<category>{c}</category>" if c else "")
        + "</item>"
        for t, d, h, c in items
    )
    return f'<?xml version="1.0"?><rss><channel>{body}</channel></rss>'.encode()


def item(title, desc="Bajada.", hours_ago=1, category=None):
    return (title, desc, hours_ago, category)


FEEDS = [
    nh.Feed("Infobae", "Política", "https://infobae.test/politica"),
    nh.Feed("Clarín", "Política", "https://clarin.test/politica"),
    nh.Feed("La Nación", None, "https://lanacion.test/all"),
]


def run(responses, argv=(), feeds=FEEDS):
    """Run perform() with canned bytes per URL (an Exception value raises)."""

    def fake_fetch(url):
        value = responses[url]
        if isinstance(value, Exception):
            raise value
        return value

    with mock.patch.object(nh, "_fetch", side_effect=fake_fetch), \
            mock.patch.object(nh, "FEEDS", feeds), \
            mock.patch.object(nh, "_now", return_value=NOW), \
            redirect_stderr(io.StringIO()):
        return nh.perform(list(argv))


class TestParsing(unittest.TestCase):
    def test_html_and_entities_are_stripped_from_summaries(self):
        data = rss(item("Título", "<p>El &quot;dólar&quot; <b>subió</b>&nbsp;hoy.</p>"))
        parsed = nh.parse_feed(data, FEEDS[0])
        self.assertEqual(parsed[0].summary, 'El "dólar" subió hoy.')

    def test_long_summary_is_cut_at_a_word_with_an_ellipsis(self):
        data = rss(item("T", "palabra " * 100))
        summary = nh.parse_feed(data, FEEDS[0])[0].summary
        self.assertLessEqual(len(summary), nh.SUMMARY_CHARS + 1)
        self.assertTrue(summary.endswith("…"))
        self.assertFalse(summary[:-1].endswith(" "))

    def test_item_without_a_date_is_dropped_not_treated_as_new(self):
        data = b'<rss><channel><item><title>Sin fecha</title></item></channel></rss>'
        self.assertEqual(nh.parse_feed(data, FEEDS[0]), [])

    def test_malformed_xml_is_an_error_for_that_feed_only(self):
        code, result = run({
            FEEDS[0].url: b"<rss><channel><item>",
            FEEDS[1].url: rss(item("Milei viaja a Washington")),
            FEEDS[2].url: rss(),
        })
        self.assertEqual(code, 0)
        self.assertEqual(len(result["items"]), 1)
        self.assertEqual(result["errors"][0]["source"], "Infobae")


class TestSelection(unittest.TestCase):
    def test_old_items_are_outside_the_window(self):
        code, result = run({
            FEEDS[0].url: rss(item("Reciente", hours_ago=2), item("Vieja", hours_ago=30)),
            FEEDS[1].url: rss(), FEEDS[2].url: rss(),
        })
        self.assertEqual([i["title"] for i in result["items"]], ["Reciente"])

    def test_same_story_from_two_outlets_is_one_item_with_both_sources(self):
        code, result = run({
            FEEDS[0].url: rss(item("Milei recibió a Landau en la Casa Rosada")),
            FEEDS[1].url: rss(item("Landau se reunió con Milei en Casa Rosada")),
            FEEDS[2].url: rss(),
        })
        self.assertEqual(len(result["items"]), 1)
        self.assertEqual(sorted(result["items"][0]["sources"]), ["Clarín", "Infobae"])
        self.assertEqual(result["items"][0]["coverage"], 2)

    def test_different_stories_are_not_merged(self):
        code, result = run({
            FEEDS[0].url: rss(item("Milei recibió a Landau en la Casa Rosada")),
            FEEDS[1].url: rss(item("El dólar blue cerró en baja")),
            FEEDS[2].url: rss(),
        })
        self.assertEqual(len(result["items"]), 2)

    def test_a_story_three_outlets_cover_outranks_a_newer_single_one(self):
        code, result = run({
            FEEDS[0].url: rss(item("Paro general de la CGT el jueves", hours_ago=6),
                              item("Exclusiva menor", hours_ago=0)),
            FEEDS[1].url: rss(item("La CGT confirmó paro general el jueves", hours_ago=5)),
            FEEDS[2].url: rss(item("Paro general: la CGT para el jueves", hours_ago=4)),
        })
        self.assertIn("CGT", result["items"][0]["title"])
        self.assertEqual(result["items"][0]["coverage"], 3)

    def test_max_items_caps_the_output(self):
        many = [item(f"Noticia número {n} distinta {n * 7}") for n in range(30)]
        code, result = run({FEEDS[0].url: rss(*many), FEEDS[1].url: rss(), FEEDS[2].url: rss()},
                           argv=["--max_items=5"])
        self.assertEqual(len(result["items"]), 5)

    def test_skipped_categories_are_left_out(self):
        code, result = run({
            FEEDS[0].url: rss(), FEEDS[1].url: rss(),
            FEEDS[2].url: rss(item("Horóscopo de hoy", category="Horóscopo"),
                              item("Suba de tasas", category="Economía")),
        })
        self.assertEqual([i["title"] for i in result["items"]], ["Suba de tasas"])


class TestArguments(unittest.TestCase):
    def test_hours_out_of_range_is_refused(self):
        self.assertEqual(run({}, argv=["--hours=0"])[0], 2)
        self.assertEqual(run({}, argv=["--hours=100"])[0], 2)

    def test_max_items_out_of_range_is_refused(self):
        self.assertEqual(run({}, argv=["--max_items=0"])[0], 2)


class TestFailure(unittest.TestCase):
    def test_every_feed_failing_is_exit_1(self):
        code, _ = run({f.url: OSError("down") for f in FEEDS})
        self.assertEqual(code, 1)

    def test_main_prints_exactly_one_json_object(self):
        out = io.StringIO()
        with mock.patch.object(nh, "_fetch", return_value=rss(item("Algo pasó"))), \
                mock.patch.object(nh, "FEEDS", FEEDS[:1]), \
                mock.patch.object(nh, "_now", return_value=NOW), \
                redirect_stdout(out):
            self.assertEqual(nh.main([]), 0)
        parsed = json.loads(out.getvalue())
        self.assertTrue(parsed["ok"])
        self.assertEqual(parsed["items"][0]["title"], "Algo pasó")


if __name__ == "__main__":
    unittest.main()
