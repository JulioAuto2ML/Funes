"""funes_guardian.py — snapshots, undo, commit-confirmed (self-healing H1).

The guardian is what undoes a change when the thing that made it — Funes, or
a deploy — is the thing that broke, so what it restores is checked for real
against a temporary checkout: files back byte for byte, files the change
created removed, a directory restored whole. Health, systemctl, git and
WhatsApp are injected.
"""

import json
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import funes_guardian as fg  # noqa: E402


class Fake:
    """What the guardian reaches outside its own state directory."""

    def __init__(self):
        self.now = 1_000_000.0
        self.up = True
        self.commands = []
        self.sent = []
        self.git_head = "aaa111"
        self.dirty = ""

    def run(self, argv, cwd=None, timeout=None):
        self.commands.append(argv)
        if argv[:3] == ["git", "rev-parse", "HEAD"]:
            return 0, self.git_head + "\n"
        if argv[:3] == ["git", "status", "--porcelain"]:
            return 0, self.dirty
        return 0, ""


class GuardianTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "agents").mkdir()
        (self.root / "agents" / "researcher.yaml").write_text("name: researcher\nmax_steps: 8\n")
        (self.root / "config").mkdir()
        (self.root / "data").mkdir()
        self.fake = Fake()
        self.g = self.guardian()

    def tearDown(self):
        self.tmp.cleanup()

    def guardian(self):
        f = self.fake
        return fg.Guardian(self.root, run=f.run, now=lambda: f.now,
                           funes_up=lambda: f.up,
                           send=lambda jid, text: f.sent.append((jid, text)) or True,
                           admin_jids=lambda: ["111@lid"])

    # ── snapshot and undo ─────────────────────────────────────────────────────

    def test_undo_restores_an_edited_file(self):
        n = self.g.begin("create_agent researcher", "funes:create_agent",
                         ["agents/researcher.yaml"], restart=["funes"])
        (self.root / "agents" / "researcher.yaml").write_text("name: researcher\nmax_steps: 64\n")
        self.g.applied(n, confirm_after=600)
        ok, msg = self.g.undo(n, by="julio")
        self.assertTrue(ok, msg)
        self.assertEqual((self.root / "agents" / "researcher.yaml").read_text(),
                         "name: researcher\nmax_steps: 8\n")
        self.assertEqual(self.g.load(n)["state"], "undone")
        # The unit that reads the file is restarted, after a delay (it may be
        # the process that asked).
        self.assertTrue(any("funes" in " ".join(c) and "restart" in c for c in self.fake.commands))

    def test_undo_removes_a_file_the_change_created(self):
        n = self.g.begin("create_agent shiny", "funes:create_agent", ["agents/shiny.yaml"])
        (self.root / "agents" / "shiny.yaml").write_text("name: shiny\n")
        self.g.applied(n)
        self.assertTrue(self.g.undo(n)[0])
        self.assertFalse((self.root / "agents" / "shiny.yaml").exists())

    def test_undo_restores_a_directory_whole(self):
        n = self.g.begin("rewrite agents", "cli", ["agents"])
        (self.root / "agents" / "researcher.yaml").unlink()
        (self.root / "agents" / "extra.yaml").write_text("x")
        self.g.applied(n)
        self.assertTrue(self.g.undo(n)[0])
        self.assertTrue((self.root / "agents" / "researcher.yaml").exists())
        self.assertFalse((self.root / "agents" / "extra.yaml").exists())

    def test_undo_twice_is_a_clear_no(self):
        n = self.g.begin("x", "cli", ["agents/researcher.yaml"])
        self.g.applied(n)
        self.assertTrue(self.g.undo(n)[0])
        ok, msg = self.g.undo(n)
        self.assertFalse(ok)
        self.assertIn("ya estaba deshecho", msg)
        self.assertFalse(self.g.undo(999)[0])

    def test_paths_outside_the_checkout_and_its_own_state_are_refused(self):
        for bad in ["/etc/passwd", "../outside", "data/guardian/txns", "agents/../../x"]:
            with self.assertRaises(fg.GuardianError, msg=bad):
                self.g.begin("x", "cli", [bad])

    # ── commit-confirmed ──────────────────────────────────────────────────────

    def test_a_healthy_change_is_confirmed_after_its_window(self):
        n = self.g.begin("x", "funes:create_agent", ["agents/researcher.yaml"])
        self.g.applied(n, confirm_after=600)
        self.fake.now += 300
        self.g.watch()
        self.assertEqual(self.g.load(n)["state"], "applied")
        self.fake.now += 400
        self.g.watch()
        self.assertEqual(self.g.load(n)["state"], "confirmed")
        # Confirmed is final: undo still works on request, the watch leaves it.
        self.fake.up = False
        self.fake.now += 600
        self.g.watch()
        self.assertEqual(self.g.load(n)["state"], "confirmed")

    def test_a_change_that_breaks_funes_is_undone_by_itself(self):
        n = self.g.begin("x", "funes:create_agent", ["agents/researcher.yaml"], restart=["funes"])
        (self.root / "agents" / "researcher.yaml").write_text("{{ not yaml")
        self.g.applied(n, confirm_after=600)
        self.fake.up = False
        self.fake.now += 60
        self.g.watch()              # one failed check is not enough
        self.assertEqual(self.g.load(n)["state"], "applied")
        self.fake.now += 60
        self.g.watch()
        t = self.g.load(n)
        self.assertEqual(t["state"], "undone")
        self.assertIn("Funes", t["undo_reason"])
        self.assertIn("max_steps: 8", (self.root / "agents" / "researcher.yaml").read_text())
        self.assertTrue(any("Deshice" in text for _, text in self.fake.sent))

    def test_a_new_incident_after_the_change_counts_as_broken(self):
        db = self.root / "data" / "memory.db"
        con = sqlite3.connect(db)
        con.execute("CREATE TABLE incidents (id INTEGER PRIMARY KEY, key TEXT, summary TEXT, "
                    "opened_at INTEGER, resolved_at INTEGER)")
        con.commit()
        n = self.g.begin("x", "deploy", ["agents/researcher.yaml"])
        self.g.applied(n, confirm_after=600)
        con.execute("INSERT INTO incidents(key, summary, opened_at, resolved_at) VALUES(?,?,?,0)",
                    ("probe_services:llm", "llama-server does not answer", int(self.fake.now) + 30))
        con.commit()
        con.close()
        for _ in range(2):
            self.fake.now += 60
            self.g.watch()
        t = self.g.load(n)
        self.assertEqual(t["state"], "undone")
        self.assertIn("llama-server", t["undo_reason"])

    # 2026-11-14: the 7.20.0 deploy was undone because the TV job — which
    # runs every minute and fails whenever the TV does not answer — failed in
    # the window. It had failed the same way an hour before the deploy. A key
    # with incidents before the change is the system's weather, not the change.
    def test_an_incident_that_also_happened_before_the_change_is_not_the_change(self):
        db = self.root / "data" / "memory.db"
        con = sqlite3.connect(db)
        con.execute("CREATE TABLE incidents (id INTEGER PRIMARY KEY, key TEXT, summary TEXT, "
                    "opened_at INTEGER, resolved_at INTEGER)")
        con.execute("INSERT INTO incidents(key, summary, opened_at, resolved_at) VALUES(?,?,?,?)",
                    ("jobs:5", "arte y clima failed", int(self.fake.now) - 3600, int(self.fake.now) - 3000))
        con.commit()
        n = self.g.begin("deploy", "deploy", ["agents/researcher.yaml"])
        self.g.applied(n, confirm_after=600)
        con.execute("INSERT INTO incidents(key, summary, opened_at, resolved_at) VALUES(?,?,?,0)",
                    ("jobs:5", "arte y clima failed", int(self.fake.now) + 30))
        con.commit()
        con.close()
        for _ in range(11):
            self.fake.now += 60
            self.g.watch()
        self.assertEqual(self.g.load(n)["state"], "confirmed")

    def test_a_begun_change_never_applied_is_abandoned_not_undone(self):
        n = self.g.begin("x", "funes:create_agent", ["agents/researcher.yaml"])
        (self.root / "agents" / "researcher.yaml").write_text("changed")
        self.fake.now += 2 * 3600
        self.g.watch()
        self.assertEqual(self.g.load(n)["state"], "abandoned")
        self.assertEqual((self.root / "agents" / "researcher.yaml").read_text(), "changed")

    def test_funes_changes_are_announced_with_their_number(self):
        n = self.g.begin("create_agent researcher", "funes:create_agent", ["agents/researcher.yaml"])
        self.g.applied(n)
        self.assertEqual(len(self.fake.sent), 1)
        self.assertIn(f"deshacer {n}", self.fake.sent[0][1])

    # ── deploy ────────────────────────────────────────────────────────────────

    def test_deploy_refuses_a_dirty_checkout(self):
        self.fake.dirty = " M src/core/agent.cpp\n"
        with self.assertRaises(fg.GuardianError):
            self.g.deploy()

    def test_undoing_a_deploy_resets_git_and_restores_the_binary(self):
        (self.root / "bin").mkdir()
        (self.root / "bin" / "funes").write_bytes(b"old binary")
        self.fake.git_head = "aaa111"

        def run(argv, cwd=None, timeout=None):
            if argv[:2] == ["git", "pull"]:
                self.fake.git_head = "bbb222"
            if argv[:2] == ["cmake", "--build"]:
                (self.root / "bin" / "funes").write_bytes(b"new binary")
            return Fake.run(self.fake, argv, cwd, timeout)
        self.fake.run = run
        g = self.guardian()
        n = g.deploy(confirm_after=600)
        self.assertEqual(g.load(n)["state"], "applied")
        self.assertEqual(g.load(n)["git_head"], "aaa111")
        self.assertTrue(g.undo(n)[0])
        self.assertEqual((self.root / "bin" / "funes").read_bytes(), b"old binary")
        self.assertIn(["git", "reset", "--hard", "aaa111"], self.fake.commands)

    def test_deploy_with_nothing_new_is_not_a_change(self):
        n = self.g.deploy()
        self.assertIsNone(n)

    # ── "deshacer N" over WhatsApp ────────────────────────────────────────────

    def test_parse_undo(self):
        self.assertEqual(fg.parse_undo("deshacer 3"), 3)
        self.assertEqual(fg.parse_undo("  Deshacer #12. "), 12)
        self.assertEqual(fg.parse_undo("undo 4"), 4)
        self.assertIsNone(fg.parse_undo("deshacer el cambio 3"))
        self.assertIsNone(fg.parse_undo("aprobar 3"))

    def test_whatsapp_undo_from_an_admin_only(self):
        n = self.g.begin("x", "funes:create_agent", ["agents/researcher.yaml"])
        (self.root / "agents" / "researcher.yaml").write_text("changed")
        self.g.applied(n)
        store = self.root / "messages.db"
        con = sqlite3.connect(store)
        con.execute("CREATE TABLE messages (id TEXT, chat_jid TEXT, content TEXT, timestamp TIMESTAMP, "
                    "is_from_me BOOLEAN)")
        con.execute("INSERT INTO messages VALUES ('m1', '999@lid', ?, '2099-01-01 10:00:00-03:00', 0)",
                    (f"deshacer {n}",))
        con.commit()
        self.g.poll_whatsapp(store)
        self.assertEqual(self.g.load(n)["state"], "applied")      # not an admin
        con.execute("INSERT INTO messages VALUES ('m2', '111@lid', ?, '2099-01-01 10:01:00-03:00', 0)",
                    (f"deshacer {n}",))
        con.commit()
        con.close()
        self.g.poll_whatsapp(store)
        self.assertEqual(self.g.load(n)["state"], "undone")
        self.g.poll_whatsapp(store)                              # seen once, handled once
        replies = [t for j, t in self.fake.sent if j == "111@lid" and "#" + str(n) in t]
        self.assertGreaterEqual(len(replies), 1)

    def test_list_shows_recent_changes(self):
        a = self.g.begin("first", "cli", ["agents/researcher.yaml"])
        self.g.applied(a)
        rows = self.g.list()
        self.assertEqual(rows[0]["id"], a)
        self.assertEqual(json.loads(json.dumps(rows))[0]["reason"], "first")


if __name__ == "__main__":
    unittest.main()
