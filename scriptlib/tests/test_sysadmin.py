"""Unit tests for the sysadmin agent's scripts (OS plan, phase 7).

prune_backups is the one that deletes, so it is run for real against a
temporary directory: what goes, what is always kept, what a dry run leaves.
The others are checked for what they refuse before anything reaches
systemctl or journalctl, with subprocess stubbed.
"""

import io
import os
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import journal  # noqa: E402
import prune_backups  # noqa: E402
import restart_unit  # noqa: E402
import unit_status  # noqa: E402

DAY = 86400


def run_main(module, argv, env=None):
    out = io.StringIO()
    with mock.patch.object(sys, "argv", [module.__name__] + argv), \
         mock.patch.dict(os.environ, env or {}), redirect_stdout(out):
        module.main()
    return out.getvalue()


class PruneBackups(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        now = time.time()
        # Ages in days: two recent, three old, one old directory.
        for name, age in [("new1", 1), ("new2", 2), ("old1", 40), ("old2", 50), ("old3", 60)]:
            p = self.dir / name
            p.write_text("x" * 1024)
            os.utime(p, (now - age * DAY, now - age * DAY))
        d = self.dir / "olddir"
        d.mkdir()
        (d / "inner").write_text("y")
        os.utime(d, (now - 90 * DAY, now - 90 * DAY))
        self.env = {"BACKUPS_DIR": str(self.dir)}

    def tearDown(self):
        self.tmp.cleanup()

    def names(self):
        return sorted(p.name for p in self.dir.iterdir())

    def test_deletes_old_entries_only(self):
        out = run_main(prune_backups, ["--older_than_days=30", "--keep=1"], self.env)
        self.assertEqual(self.names(), ["new1", "new2"])
        self.assertIn("deleted  olddir", out)
        self.assertIn("4 removed, 2 kept", out)

    def test_always_keeps_the_newest(self):
        # Everything is older than 7 days except new1/new2; keep=4 protects
        # the four newest whatever their age.
        run_main(prune_backups, ["--older_than_days=7", "--keep=4"], self.env)
        self.assertEqual(self.names(), ["new1", "new2", "old1", "old2"])

    def test_dry_run_deletes_nothing(self):
        before = self.names()
        out = run_main(prune_backups, ["--older_than_days=30", "--dry_run"], self.env)
        self.assertEqual(self.names(), before)
        self.assertIn("would delete", out)
        self.assertIn("would free", out)

    def test_refuses_a_short_window(self):
        with self.assertRaises(SystemExit) as e:
            run_main(prune_backups, ["--older_than_days=1"], self.env)
        self.assertIn("at least 7", str(e.exception))
        with self.assertRaises(SystemExit):
            run_main(prune_backups, ["--keep=0"], self.env)
        self.assertEqual(len(self.names()), 6)

    def test_needs_its_directory(self):
        with self.assertRaises(SystemExit) as e:
            run_main(prune_backups, [], {"BACKUPS_DIR": str(self.dir / "missing")})
        self.assertIn("no backups directory", str(e.exception))


class Journal(unittest.TestCase):
    def call(self, argv):
        with mock.patch("subprocess.run") as run:
            run.return_value = mock.Mock(returncode=0, stdout="a line\n", stderr="")
            run_main(journal, argv)
            return run.call_args[0][0]

    def test_relative_since_and_line_cap(self):
        cmd = self.call(["--unit=whatsapp-bridge", "--since=2h", "--lines=5000"])
        self.assertEqual(cmd[cmd.index("-u") + 1], "whatsapp-bridge")
        self.assertEqual(cmd[cmd.index("--since") + 1], "-2 hours")
        self.assertEqual(cmd[cmd.index("-n") + 1], "150")

    def test_lines_are_compacted(self):
        raw = ("2026-11-12T08:52:44-03:00 yoda whatsapp-bridge[1252]: hello there\n"
               "2026-11-12T08:52:45-03:00 yoda funes[9]: " + "x" * 500 + "\n")
        with mock.patch("subprocess.run") as run:
            run.return_value = mock.Mock(returncode=0, stdout=raw, stderr="")
            one = run_main(journal, ["--unit=whatsapp-bridge"]).splitlines()
            every = run_main(journal, []).splitlines()
        self.assertEqual(one[0], "11-12 08:52:44 hello there")
        self.assertEqual(every[0], "11-12 08:52:44 whatsapp-bridge: hello there")
        self.assertTrue(every[1].endswith(" …") and len(every[1]) <= 250)

    def test_refuses_option_shaped_values(self):
        for argv in (["--unit=-f"], ["--unit=a b"], ["--since=--follow"], ["--since=x;y"]):
            with self.assertRaises(SystemExit, msg=argv), mock.patch("subprocess.run") as run:
                run_main(journal, argv)
            run.assert_not_called()


class UnitStatus(unittest.TestCase):
    def test_refuses_option_shaped_units(self):
        with self.assertRaises(SystemExit), mock.patch("subprocess.run") as run:
            run_main(unit_status, ["--unit=--all"])
        run.assert_not_called()

    def test_reports_a_missing_unit(self):
        with mock.patch("subprocess.run") as run:
            run.return_value = mock.Mock(returncode=0, stdout="LoadState=not-found\n", stderr="")
            out = run_main(unit_status, ["--unit=nope"])
        self.assertIn("nope.service: no such user unit", out)


class RestartUnit(unittest.TestCase):
    def test_only_the_listed_units(self):
        for unit in ("funes", "dbus", "", "whatsapp-bridge;reboot"):
            with self.assertRaises(SystemExit), mock.patch("subprocess.run") as run:
                run_main(restart_unit, [f"--unit={unit}"])
            run.assert_not_called()

    def test_fails_when_the_unit_does_not_come_back(self):
        states = iter(["ActiveState=active\nSubState=running\n", "",
                       "ActiveState=failed\nSubState=failed\n"])
        with mock.patch("subprocess.run") as run, mock.patch("time.sleep"):
            run.side_effect = lambda *a, **k: mock.Mock(returncode=0, stdout=next(states), stderr="")
            with self.assertRaises(SystemExit) as e:
                run_main(restart_unit, ["--unit=whatsapp-bridge"])
        self.assertEqual(e.exception.code, 1)


if __name__ == "__main__":
    unittest.main()
