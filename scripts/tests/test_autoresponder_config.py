"""Where whatsapp_autoresponder.py looks for funes.conf / funes.local.

The 5.1 split moved this script into funes-julio while the configuration
stayed in the Funes checkout. Resolving config/ relative to __file__ then
found a directory that exists but holds no funes.local, so every setting fell
back to its default and the service exited on "WHATSAPP_WHITELIST is empty"
with the variable plainly set in the file it was supposed to read. These tests
pin the search order so that cannot come back silently.
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import whatsapp_autoresponder as wa  # noqa: E402


class ConfigDirTest(unittest.TestCase):
    def setUp(self):
        self._cwd = Path.cwd()
        self._env = os.environ.get("FUNES_CONFIG_DIR")
        os.environ.pop("FUNES_CONFIG_DIR", None)
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self):
        os.chdir(self._cwd)
        if self._env is None:
            os.environ.pop("FUNES_CONFIG_DIR", None)
        else:
            os.environ["FUNES_CONFIG_DIR"] = self._env
        self._tmp.cleanup()

    def _checkout(self, name, *files):
        d = self.root / name / "config"
        d.mkdir(parents=True)
        for f in files:
            (d / f).write_text("WHATSAPP_WHITELIST=1@s.whatsapp.net\n")
        return d.parent

    def test_the_working_directory_wins_over_the_script_s_own_repo(self):
        """The systemd unit's WorkingDirectory is the Funes checkout; that is
        the whole mechanism by which the moved script still finds funes.local."""
        funes = self._checkout("Funes-v4", "funes.conf", "funes.local")
        os.chdir(funes)
        self.assertEqual(wa.config_dir(), funes / "config")

    def test_funes_config_dir_overrides_everything(self):
        funes = self._checkout("Funes-v4", "funes.local")
        elsewhere = self._checkout("elsewhere", "funes.local")
        os.chdir(funes)
        os.environ["FUNES_CONFIG_DIR"] = str(elsewhere / "config")
        self.assertEqual(wa.config_dir(), elsewhere / "config")

    def test_a_config_dir_without_either_file_is_not_chosen(self):
        """The regression itself: a directory that exists but is empty must
        not end the search, or every setting silently takes its default."""
        empty = self.root / "funes-julio"
        (empty / "config").mkdir(parents=True)
        os.chdir(empty)
        self.assertNotEqual(wa.config_dir(), empty / "config")

    def test_falls_back_to_this_repository_for_a_bare_checkout(self):
        os.chdir(self.root)          # no config/ anywhere above
        self.assertEqual(wa.config_dir(), wa.REPO_ROOT / "config")


if __name__ == "__main__":
    unittest.main()
