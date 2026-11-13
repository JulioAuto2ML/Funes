"""Unit tests for linkedin_token.py.

LinkedIn itself is not called: the exchange and the WhatsApp send are passed
in. What is tested is what the public callback must never get wrong — it acts
only on the one pending state, once — and that the reminder fires inside the
warning window and not before.
"""

import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import linkedin_token as lt

NOW = 1_800_000_000.0
CFG = {"LINKEDIN_REDIRECT_URI": "https://yoda.example.ts.net/linkedin/callback",
       "LINKEDIN_ALERT_JID": "123@lid", "WHATSAPP_BRIDGE_URL": "http://bridge"}


def write_env(d: Path, expiry: float):
    (d / ".env").write_text(
        "# LinkedIn\nLI_CLIENT_ID=cid\nLI_CLIENT_SECRET=secret\n"
        f"LI_ACCESS_TOKEN=old\nLI_REFRESH_TOKEN=\nLI_TOKEN_EXPIRY={int(expiry)}\n"
        "GMAIL_ADDRESS=x@example.com\n")


class Check(unittest.TestCase):
    def setUp(self):
        self.tmp = TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.sent = []

    def tearDown(self):
        self.tmp.cleanup()

    def send(self, bridge, jid, text):
        self.sent.append((bridge, jid, text))
        return True

    def test_quiet_outside_the_window(self):
        write_env(self.dir, NOW + 20 * 86400)
        self.assertEqual(lt.cmd_check(self.dir, CFG, NOW, send=self.send), 0)
        self.assertEqual(self.sent, [])

    def test_alerts_inside_the_window_with_a_link_carrying_state(self):
        write_env(self.dir, NOW + 3 * 86400)
        self.assertEqual(lt.cmd_check(self.dir, CFG, NOW, send=self.send), 0)
        (bridge, jid, text), = self.sent
        self.assertEqual((bridge, jid), ("http://bridge", "123@lid"))
        state = json.loads(lt.state_path(self.dir).read_text())["state"]
        self.assertIn("state=" + state, text)
        self.assertIn("redirect_uri=https%3A%2F%2Fyoda.example.ts.net", text)
        self.assertIn("3 día", text)

    def test_alerts_once_expired(self):
        write_env(self.dir, NOW - 3600)
        lt.cmd_check(self.dir, CFG, NOW, send=self.send)
        self.assertIn("venció", self.sent[0][2])

    def test_the_next_day_reuses_the_pending_link(self):
        write_env(self.dir, NOW + 3 * 86400)
        lt.cmd_check(self.dir, CFG, NOW, send=self.send)
        lt.cmd_check(self.dir, CFG, NOW + 86400, send=self.send)
        self.assertEqual(self.sent[0][2].split("state=")[1][:43],
                         self.sent[1][2].split("state=")[1][:43])

    def test_a_failed_send_is_a_failure(self):
        write_env(self.dir, NOW + 86400)
        self.assertEqual(lt.cmd_check(self.dir, CFG, NOW, send=lambda *a: False), 1)


class Callback(unittest.TestCase):
    def setUp(self):
        self.tmp = TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        write_env(self.dir, NOW - 3600)
        self.state = lt.new_or_pending_state(self.dir, NOW)
        self.calls = []

    def tearDown(self):
        self.tmp.cleanup()

    def exchange(self, code, creds, redirect):
        self.calls.append((code, creds["LI_CLIENT_SECRET"], redirect))
        return {"access_token": "new-token", "expires_in": 60 * 86400}

    def callback(self, **query):
        return lt.handle_callback({k: [v] for k, v in query.items()},
                                  self.dir, CFG, NOW + 60, exchange=self.exchange)

    def test_the_pending_state_renews_the_token(self):
        status, msg = self.callback(code="abc", state=self.state)
        self.assertEqual(status, 200)
        self.assertEqual(self.calls, [("abc", "secret", CFG["LINKEDIN_REDIRECT_URI"])])
        env = lt.load_kv(self.dir / ".env")
        self.assertEqual(env["LI_ACCESS_TOKEN"], "new-token")
        self.assertEqual(int(env["LI_TOKEN_EXPIRY"]), int(NOW + 60) + 60 * 86400)
        self.assertEqual(env["GMAIL_ADDRESS"], "x@example.com")     # the rest untouched
        self.assertIn("# LinkedIn", (self.dir / ".env").read_text())

    def test_a_state_is_spent_on_first_use(self):
        self.callback(code="abc", state=self.state)
        status, _ = self.callback(code="abc", state=self.state)
        self.assertEqual(status, 404)
        self.assertEqual(len(self.calls), 1)

    def test_a_wrong_or_missing_state_does_nothing(self):
        for query in ({"code": "abc", "state": "guess"}, {"code": "abc"}):
            status, _ = self.callback(**query)
            self.assertEqual(status, 404)
        self.assertEqual(self.calls, [])
        self.assertEqual(lt.load_kv(self.dir / ".env")["LI_ACCESS_TOKEN"], "old")

    def test_an_old_state_has_expired(self):
        status, _ = lt.handle_callback({"code": ["abc"], "state": [self.state]},
                                       self.dir, CFG, NOW + lt.STATE_TTL + 1,
                                       exchange=self.exchange)
        self.assertEqual(status, 404)
        self.assertEqual(self.calls, [])

    def test_nothing_from_the_request_reaches_the_page(self):
        _, msg = self.callback(code="<script>x</script>", state="<b>evil</b>")
        self.assertNotIn("<", msg)

    def test_a_refusal_keeps_the_old_token(self):
        status, _ = self.callback(error="user_cancelled_authorize", state=self.state)
        self.assertEqual(status, 400)
        self.assertEqual(self.calls, [])
        self.assertEqual(lt.load_kv(self.dir / ".env")["LI_ACCESS_TOKEN"], "old")


if __name__ == "__main__":
    unittest.main()
