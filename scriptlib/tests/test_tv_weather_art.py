"""Unit tests for tv_weather_art.py and tv_canvas.py.

Everything that leaves the machine is stubbed — the Met, Open-Meteo, the LLM
and the TV — so what is under test is the script's own decisions: which
artwork it accepts, that one is chosen per day and not per hour, that a bad
translation falls back to the original instead of reaching the wall, and that
a sleeping set on a scheduled run is a quiet "not shown", not a failure.

Needs Pillow (scriptlib/requirements.txt); skipped without it.
"""

import io
import json
import os
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from datetime import date
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

if Image is not None:
    import tv_canvas
    import tv_weather_art as twa

DAY = date(2026, 10, 21)          # spring in Villa Adelina

WEATHER = {
    "current": {"temperature_2m": 19.6, "apparent_temperature": 17.9,
                "relative_humidity_2m": 62, "weather_code": 0},
    "daily": {
        "time": ["2026-10-21", "2026-10-22", "2026-10-23", "2026-10-24", "2026-10-25"],
        "weather_code": [53, 81, 53, 3, 0],
        "temperature_2m_max": [20.4, 21.2, 22.6, 19.4, 18.2],
        "temperature_2m_min": [13.1, 14.4, 15.6, 13.1, 11.4],
        "precipitation_probability_max": [47, 79, 75, 39, 55],
    },
}


def met_object(oid, w_cm=80, h_cm=45, pd=True, image=True, **extra):
    o = {
        "objectID": oid, "isPublicDomain": pd,
        "primaryImage": f"https://img/{oid}.jpg" if image else "",
        "title": f"Work {oid}", "artistDisplayName": f"Artist {oid}",
        "artistDisplayBio": "French, 1840–1926", "objectDate": "1880",
        "medium": "Oil on canvas", "period": "", "culture": "",
        "measurements": [{"elementName": "Overall",
                          "elementMeasurements": {"Height": h_cm, "Width": w_cm}}],
    }
    o.update(extra)
    return o


class FakeMet:
    """search(query) -> ids; obj(id) -> dict; image(url) -> JPEG bytes."""

    def __init__(self, objects, by_query=None, pixels=(3000, 1700)):
        self.objects = {o["objectID"]: o for o in objects}
        self.by_query = by_query or {}
        self.pixels = pixels
        self.queries = []
        self.downloads = []

    def search(self, query):
        self.queries.append(query)
        return self.by_query.get(query, list(self.objects))

    def obj(self, oid):
        return self.objects[oid]

    def image(self, url):
        self.downloads.append(url)
        buf = io.BytesIO()
        w, h = self.pixels.get(url, (3000, 1700)) if isinstance(self.pixels, dict) else self.pixels
        Image.new("RGB", (w, h), (120, 110, 90)).save(buf, "JPEG")
        return buf.getvalue()


@unittest.skipIf(Image is None, "Pillow not installed")
class TestSeasonAndQueries(unittest.TestCase):
    def test_southern_hemisphere_seasons(self):
        self.assertEqual(twa.season(date(2026, 10, 21)), "primavera")
        self.assertEqual(twa.season(date(2026, 1, 10)), "verano")
        self.assertEqual(twa.season(date(2026, 4, 10)), "otoño")
        self.assertEqual(twa.season(date(2026, 7, 10)), "invierno")

    def test_queries_combine_then_fall_back(self):
        qs = twa.queries("drizzle", "primavera")
        self.assertIn("spring rain", qs[:2])
        self.assertTrue(any(q == "rain" or q.endswith(" rain") for q in qs))
        self.assertTrue(any("spring" in q and "rain" not in q for q in qs))
        self.assertEqual(len(qs), len(set(qs)))

    def test_weather_kind(self):
        self.assertEqual(tv_canvas.kind(0), "sun")
        self.assertEqual(tv_canvas.kind(53), "drizzle")
        self.assertEqual(tv_canvas.kind(81), "rain")
        self.assertEqual(tv_canvas.kind(95), "storm")


@unittest.skipIf(Image is None, "Pillow not installed")
class TestPick(unittest.TestCase):
    def test_rejects_what_would_look_bad_or_is_not_ours_to_show(self):
        met = FakeMet([
            met_object(1, pd=False),                 # not public domain
            met_object(2, image=False),              # no image
            met_object(3, w_cm=30, h_cm=90),         # hanging scroll
            met_object(4),                           # fine on paper, small in pixels
            met_object(5),                           # fine
        ], pixels={"https://img/4.jpg": (1200, 700), "https://img/5.jpg": (3200, 1800)})
        art = twa.pick_artwork(met, ["rain"], DAY, history=[])
        self.assertEqual(art["objectID"], 5)
        self.assertNotIn("https://img/1.jpg", met.downloads)
        self.assertNotIn("https://img/3.jpg", met.downloads)

    def test_skips_recently_shown(self):
        met = FakeMet([met_object(5), met_object(6)])
        self.assertEqual(twa.pick_artwork(met, ["rain"], DAY, history=[5])["objectID"], 6)

    def test_same_day_same_choice(self):
        objs = [met_object(i) for i in range(10, 30)]
        a = twa.pick_artwork(FakeMet(objs), ["rain"], DAY, history=[])
        b = twa.pick_artwork(FakeMet(objs), ["rain"], DAY, history=[])
        self.assertEqual(a["objectID"], b["objectID"])

    def test_falls_through_queries_until_one_has_a_usable_work(self):
        met = FakeMet([met_object(7)], by_query={"spring rain": [], "rain": [7]})
        art = twa.pick_artwork(met, ["spring rain", "rain"], DAY, history=[])
        self.assertEqual(art["objectID"], 7)
        self.assertEqual(met.queries[:2], ["spring rain", "rain"])

    def test_nothing_usable_is_an_error_not_a_blank_wall(self):
        met = FakeMet([met_object(1, pd=False)])
        with self.assertRaises(twa.NoArtwork):
            twa.pick_artwork(met, ["rain"], DAY, history=[])


@unittest.skipIf(Image is None, "Pillow not installed")
class TestTranslation(unittest.TestCase):
    META = {"title": "Misty Rain on the River in Spring", "bio": "Chinese, died 1916",
            "detail": "Qing dynasty · Ink on gold-flecked paper"}

    def test_valid_reply_is_used(self):
        reply = json.dumps({"title": "Lluvia brumosa sobre el río en primavera",
                            "bio": "China, murió en 1916",
                            "detail": "Dinastía Qing · Tinta sobre papel con pan de oro"})
        out, ok = twa.translate(self.META, lambda prompt: reply)
        self.assertTrue(ok)
        self.assertEqual(out["bio"], "China, murió en 1916")

    def test_thinking_and_fences_are_stripped(self):
        reply = "<think>hmm</think>\n```json\n" + json.dumps(
            {"title": "T", "bio": "B", "detail": "D"}) + "\n```"
        out, ok = twa.translate(self.META, lambda prompt: reply)
        self.assertTrue(ok)
        self.assertEqual(out["title"], "T")

    def test_llm_down_keeps_the_original(self):
        def down(prompt):
            raise OSError("connection refused")
        out, ok = twa.translate(self.META, down)
        self.assertFalse(ok)
        self.assertEqual(out, self.META)

    def test_bad_shapes_keep_the_original(self):
        for reply in ["not json", json.dumps({"title": "only one"}),
                      json.dumps({"title": "x" * 500, "bio": "b", "detail": "d"}),
                      json.dumps({"title": "a\nb", "bio": "b", "detail": "d"})]:
            out, ok = twa.translate(self.META, lambda prompt, r=reply: r)
            self.assertFalse(ok, reply[:40])
            self.assertEqual(out, self.META)


@unittest.skipIf(Image is None, "Pillow not installed")
class TestLlmIdle(unittest.TestCase):
    def reply(self, body):
        r = mock.MagicMock()
        r.__enter__.return_value.read.return_value = json.dumps(body).encode()
        return r

    def test_idle_only_when_no_slot_is_processing(self):
        with mock.patch.dict(os.environ, {"FUNES_LLM_URL": "http://127.0.0.1:8080"}), \
             mock.patch.object(twa.urllib.request, "urlopen") as up:
            up.return_value = self.reply([{"id": 0, "is_processing": False}])
            self.assertTrue(twa.llm_idle())
            up.return_value = self.reply([{"id": 0, "is_processing": True}])
            self.assertFalse(twa.llm_idle())

    def test_down_or_unknown_counts_as_busy(self):
        # During run_images' window the LLM is stopped: connection refused.
        with mock.patch.dict(os.environ, {"FUNES_LLM_URL": "http://127.0.0.1:8080"}), \
             mock.patch.object(twa.urllib.request, "urlopen", side_effect=OSError("refused")):
            self.assertFalse(twa.llm_idle())
        with mock.patch.dict(os.environ, {"FUNES_LLM_URL": ""}):
            self.assertFalse(twa.llm_idle())


@unittest.skipIf(Image is None, "Pillow not installed")
class TestCanvas(unittest.TestCase):
    META = {"artist": "Wu Shixian", "bio": "China, murió en 1916",
            "title": "Lluvia brumosa sobre el río en primavera " * 3,
            "detail": "Dinastía Qing", "place": "Villa Adelina"}

    def test_landscape_and_portrait_both_fill_4k(self):
        for size in [(3000, 1700), (900, 2400)]:
            img = tv_canvas.compose(Image.new("RGB", size, (90, 80, 70)), self.META, WEATHER)
            self.assertEqual(img.size, (3840, 2160))
            self.assertEqual(img.mode, "RGB")

    def test_long_label_lines_wrap_inside_the_placard(self):
        # Seen on the first real run: a Met bio ran off the placard's edge.
        from PIL import ImageDraw
        d = ImageDraw.Draw(Image.new("RGB", (10, 10)))
        bio = "American, Boston, Massachusetts 1836–1910 Prouts Neck, Maine"
        width = tv_canvas.PANEL_W - 160
        lines = tv_canvas.wrap(d, bio, tv_canvas.font("sans", 30), width, max_lines=2)
        self.assertGreater(len(lines), 1)
        for line in lines:
            self.assertLessEqual(d.textlength(line, font=tv_canvas.font("sans", 30)), width)

    def test_missing_optional_fields_do_not_crash(self):
        meta = {"artist": "", "bio": "", "title": "Sin título", "detail": "", "place": "X"}
        img = tv_canvas.compose(Image.new("RGB", (3000, 1700)), meta, WEATHER)
        self.assertEqual(img.size, (3840, 2160))


@unittest.skipIf(Image is None, "Pillow not installed")
class TestRun(unittest.TestCase):
    def setUp(self):
        self.tmp = TemporaryDirectory()
        root = Path(self.tmp.name)
        self.ws = root / "ws"
        self.ws.mkdir()
        self.cwd = os.getcwd()
        os.chdir(self.ws)
        mock.patch.dict(os.environ, {
            "FUNES_TV_HOST": "192.0.2.10", "FUNES_TV_TOKEN_FILE": str(root / "tok"),
            "FUNES_WEATHER_LAT": "-34.51", "FUNES_WEATHER_LON": "-58.54",
            "FUNES_WEATHER_PLACE": "Villa Adelina",
        }).start()
        self.met = FakeMet([met_object(i) for i in range(1, 6)])
        mock.patch.object(twa, "Met", lambda: self.met).start()
        mock.patch.object(twa, "fetch_weather", lambda lat, lon: WEATHER).start()
        mock.patch.object(twa, "llm_complete", lambda prompt: json.dumps(
            {"title": "Obra", "bio": "Francés, 1840–1926", "detail": "1880 · Óleo sobre lienzo"})).start()
        mock.patch.object(twa, "today", lambda: DAY).start()
        mock.patch.object(twa, "RETRY_SECONDS", 0).start()
        mock.patch.object(twa, "LLM_POLL_SECONDS", 0).start()
        self.idle = mock.patch.object(twa, "llm_idle", return_value=True).start()
        self.perform = mock.patch.object(
            twa.tv_art, "perform",
            return_value=(0, {"ok": True, "content_id": "MY_F0009", "artmode": "on"})).start()

    def tearDown(self):
        mock.patch.stopall()
        os.chdir(self.cwd)
        self.tmp.cleanup()

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = twa.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def test_composes_into_the_workspace_and_shows_without_waking(self):
        code, out, err = self.run_main()
        self.assertEqual(code, 0, err)
        result = json.loads(out)
        self.assertTrue(result["shown"])
        self.assertEqual(result["artwork"]["title"], "Obra")
        argv = self.perform.call_args[0][0]
        self.assertIn("--action=show", argv)
        self.assertIn("--no-wake", argv)
        image = next(a for a in argv if a.startswith("--image="))[len("--image="):]
        self.assertFalse(os.path.isabs(image))
        self.assertEqual(Image.open(self.ws / image).size, (3840, 2160))

    def test_wake_flag_lets_tv_art_wake_the_set(self):
        self.run_main("--wake")
        self.assertNotIn("--no-wake", self.perform.call_args[0][0])

    def test_artwork_is_chosen_once_per_day(self):
        self.run_main()
        searches = len(self.met.queries)
        self.run_main()
        self.assertEqual(len(self.met.queries), searches)

    def test_refresh_picks_a_different_work(self):
        _, out1, _ = self.run_main()
        _, out2, _ = self.run_main("--refresh_art")
        self.assertNotEqual(json.loads(out1)["artwork"]["id"], json.loads(out2)["artwork"]["id"])

    def test_query_overrides_weather_and_season(self):
        self.run_main("--query=Turner")
        self.assertEqual(self.met.queries[0], "Turner")

    def test_untranslated_day_is_retried_on_the_next_run(self):
        with mock.patch.object(twa, "llm_complete", side_effect=OSError("down")):
            _, out, _ = self.run_main()
        self.assertTrue(json.loads(out)["artwork"]["title"].startswith("Work "))
        _, out, _ = self.run_main()
        self.assertEqual(json.loads(out)["artwork"]["title"], "Obra")

    # ── sharing the one LLM slot with the newsletter ──────────────────────
    # llama-server runs --parallel 1: a translation in the middle of the
    # curator's run evicts its prompt cache. So the script asks first.

    def test_scheduled_run_does_not_translate_while_the_llm_is_busy(self):
        self.idle.return_value = False
        with mock.patch.object(twa, "llm_complete") as llm:
            code, out, _ = self.run_main()
        self.assertEqual(code, 0)
        llm.assert_not_called()
        self.assertFalse(json.loads(out)["artwork"]["translated"])

    def test_busy_then_idle_translates_on_a_later_run(self):
        self.idle.return_value = False
        self.run_main()
        self.idle.return_value = True
        _, out, _ = self.run_main()
        self.assertEqual(json.loads(out)["artwork"]["title"], "Obra")

    def test_a_person_waits_for_the_llm_to_free_up(self):
        self.idle.side_effect = [False, False, True]
        _, out, _ = self.run_main("--wake")
        self.assertTrue(json.loads(out)["artwork"]["translated"])
        self.assertEqual(self.idle.call_count, 3)

    def test_a_person_does_not_wait_forever(self):
        self.idle.return_value = False
        with mock.patch.object(twa, "LLM_WAIT_SECONDS", 0), \
             mock.patch.object(twa, "llm_complete") as llm:
            code, out, _ = self.run_main("--refresh_art")
        self.assertEqual(code, 0)
        llm.assert_not_called()
        self.assertFalse(json.loads(out)["artwork"]["translated"])

    def test_sleeping_tv_is_a_quiet_not_shown(self):
        self.perform.return_value = (twa.tv_art.EXIT_UNREACHABLE, None)
        code, out, _ = self.run_main()
        self.assertEqual(code, 0)
        result = json.loads(out)
        self.assertFalse(result["shown"])
        self.assertIn("asleep", result["reason"])

    def test_other_tv_failures_are_failures(self):
        self.perform.return_value = (1, None)
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        self.assertEqual(out, "")

    def test_one_weather_hiccup_is_retried(self):
        # Seen on yoda's first real run: one TLS handshake timeout to
        # Open-Meteo (yoda reaches the internet through luke), fine a minute later.
        calls = []
        def flaky(lat, lon):
            calls.append(1)
            if len(calls) == 1:
                raise OSError("handshake timed out")
            return WEATHER
        with mock.patch.object(twa, "fetch_weather", flaky):
            code, out, err = self.run_main()
        self.assertEqual(code, 0, err)
        self.assertEqual(len(calls), 2)

    def test_weather_unavailable_fails_loudly(self):
        with mock.patch.object(twa, "fetch_weather", side_effect=OSError("timeout")):
            code, out, err = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("weather", err)

    def test_missing_location_is_an_installation_fault(self):
        del os.environ["FUNES_WEATHER_LAT"]
        code, _, err = self.run_main()
        self.assertEqual(code, 3)
        self.assertIn("FUNES_WEATHER_LAT", err)


if __name__ == "__main__":
    unittest.main()
