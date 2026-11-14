"""Unit tests for the self-healing probes (H0, src/core/probes.h).

Each probe prints {"checks": [...]}; what is checked here is what each check
says in the cases the incidents came from — an expired token, posts that did
not go out, no internet — and that what does not apply is left out rather
than reported broken.
"""

import json
import sys
import tempfile
import time
import unittest
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import probe_internet  # noqa: E402
import probe_newsletter  # noqa: E402
import probe_services  # noqa: E402

DAY = 86400
# A Monday (EN days are Mon/Wed/Fri), 15:10 local: the 15:00 post is still
# within its grace period.
MONDAY_1510 = datetime(2026, 11, 16, 15, 10).timestamp()

CRONTAB = """\
00 13 * * 1,3,5 /usr/bin/python3 /x/publishing/post_tweet.py 1  --dir /x >> /x/post_log.txt 2>&1
00 14 * * 1,3,5 /usr/bin/python3 /x/publishing/post_tweet.py 2  --dir /x >> /x/post_log.txt 2>&1
00 15 * * 1,3,5 /usr/bin/python3 /x/publishing/post_tweet.py 3  --dir /x >> /x/post_log.txt 2>&1
00 16 * * 1,3,5 /usr/bin/python3 /x/publishing/post_tweet.py 4  --dir /x >> /x/post_log.txt 2>&1
30 13 * * 2,4,6 /usr/bin/python3 /x/publishing/post_tweet.py 1 --publication ai-pulse-es --dir /x
# 00 12 * * 1 /usr/bin/python3 /x/publishing/post_tweet.py 9 --dir /x
"""


class Newsletter(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.crontab = self.dir / "crontab"
        self.crontab.write_text(CRONTAB)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, rel, data):
        p = self.dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(data if isinstance(data, str) else json.dumps(data))

    def issue(self, items=4, withdrawn=(), status="sent", pub="ai-pulse", day="2026-11-16"):
        self.write(f"runs/{pub}/{day}.json", {"status": status})
        self.write(f"issues/{pub}/{day}.json",
                   {"items": [{"text": f"#{i}", **({"withdrawn": "x"} if i in withdrawn else {})}
                              for i in range(1, items + 1)]})

    def checks(self, now=MONDAY_1510):
        return {c["name"]: c for c in probe_newsletter.collect(self.dir, self.crontab, now)}

    def test_token_valid_expiring_expired(self):
        self.write(".env", f"LI_ACCESS_TOKEN=secret\nLI_TOKEN_EXPIRY={int(MONDAY_1510 + 30 * DAY)}\n")
        c = self.checks()["linkedin_token"]
        self.assertTrue(c["ok"])
        self.assertNotIn("secret", json.dumps(c))

        self.write(".env", f"LI_TOKEN_EXPIRY={int(MONDAY_1510 + DAY)}\n")
        c = self.checks()["linkedin_token"]
        self.assertFalse(c["ok"])
        self.assertEqual(c["severity"], "warning")

        self.write(".env", f"LI_TOKEN_EXPIRY={int(MONDAY_1510 - DAY)}\n")
        c = self.checks()["linkedin_token"]
        self.assertFalse(c["ok"])
        self.assertEqual(c["severity"], "critical")
        self.assertIn("expired", c["summary"])

    def test_no_token_configured_is_no_check(self):
        self.assertNotIn("linkedin_token", self.checks())

    # The 2026-11-13 shape: the issue went out, the 13:00 and 14:00 posts did not.
    def test_posts_due_but_not_posted(self):
        self.issue()
        self.write("post_log.txt", "Posting #1\nERROR: LinkedIn access token has expired (valid for 60 days).\n")
        c = self.checks()["linkedin_posts"]
        self.assertFalse(c["ok"])
        # 13:00 and 14:00 are owed; 15:00 is still within its grace period.
        self.assertIn("2 of 2", c["summary"])
        self.assertIn("ai-pulse #1", c["summary"])
        self.assertIn("token has expired", c["evidence"])

    def test_posts_that_went_out(self):
        self.issue()
        self.write("posted/ai-pulse/2026-11-16.json", {"1": {"post_id": "a"}, "2": {"post_id": "b"}})
        self.assertTrue(self.checks()["linkedin_posts"]["ok"])

    def test_withdrawn_and_missing_items_are_not_owed(self):
        self.issue(items=1, withdrawn=(1,))
        self.assertNotIn("linkedin_posts", self.checks())

    def test_no_issue_today_is_not_this_probes_failure(self):
        self.issue(status="blocked")
        self.assertNotIn("linkedin_posts", self.checks())
        self.assertNotIn("linkedin_posts", self.checks(now=MONDAY_1510 + DAY))   # Tuesday: ES day, no ES issue

    def test_cron_dow(self):
        self.assertTrue(probe_newsletter.cron_dow_matches("1,3,5", 3))
        self.assertFalse(probe_newsletter.cron_dow_matches("1,3,5", 2))
        self.assertTrue(probe_newsletter.cron_dow_matches("1-5", 4))
        self.assertTrue(probe_newsletter.cron_dow_matches("7", 0))
        self.assertTrue(probe_newsletter.cron_dow_matches("*", 6))


class Internet(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.route = Path(self.tmp.name) / "route"
        self.route.write_text(
            "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
            "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
            "enp12s0\t00000000\t01000D11\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
            "enp12s0\t00000D11\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n")

    def tearDown(self):
        self.tmp.cleanup()

    def test_default_routes_best_first(self):
        self.assertEqual(probe_internet.default_routes(str(self.route)), ["enp12s0", "wlan0"])

    def test_one_host_answering_is_enough(self):
        answers = {"a": "a: timeout", "b": ""}
        c = probe_internet.collect(["a", "b"], str(self.route), lambda h: answers[h])[0]
        self.assertTrue(c["ok"])

    # The 2026-11-14 shape: nothing answers, the default route is the wired link.
    def test_no_internet_names_the_route(self):
        c = probe_internet.collect(["a", "b"], str(self.route), lambda h: f"{h}: No route to host")[0]
        self.assertFalse(c["ok"])
        self.assertIn("via enp12s0, wlan0", c["summary"])
        self.assertIn("No route to host", c["evidence"])


class Services(unittest.TestCase):
    def test_unconfigured_services_are_left_out(self):
        self.assertEqual(probe_services.collect({}), [])

    def test_base_url(self):
        self.assertEqual(probe_services.base_url("http://localhost:8080/v1"), "http://localhost:8080")
        self.assertEqual(probe_services.base_url(""), "")

    def test_a_service_that_does_not_answer(self):
        # Port 9 (discard) on localhost: nothing listens there in a test run.
        c = probe_services.collect({"LLM_URL": "http://127.0.0.1:9"})[0]
        self.assertEqual(c["name"], "llm")
        self.assertFalse(c["ok"])
        self.assertEqual(c["severity"], "critical")

    def test_disk(self):
        with tempfile.TemporaryDirectory() as d:
            c = probe_services.collect({"DATA_DIR": d})[0]
            self.assertEqual(c["name"], "disk")
            self.assertIn("free", c["summary"])


if __name__ == "__main__":
    unittest.main()
