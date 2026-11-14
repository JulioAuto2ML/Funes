"""Messages the autoresponder answers or drops without asking Funes.

"/new" and "/clear" rotate the session; "deshacer N" is left to the guardian
(funes_guardian.py reads it from the store itself, so it works while Funes is
down) — forwarded as well, one undo would be done and answered twice.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import whatsapp_autoresponder as wa  # noqa: E402


class LocalCommandTest(unittest.TestCase):
    def test_reset(self):
        for text in ["/new", "/clear", " /CLEAR "]:
            self.assertEqual(wa.local_command(text), "reset", text)

    def test_undo_is_the_guardians(self):
        for text in ["deshacer 3", "Deshacer #12.", "undo 4"]:
            self.assertEqual(wa.local_command(text), "undo", text)

    def test_everything_else_goes_to_funes(self):
        for text in ["deshacer el cambio 3", "/clear the list", "aprobar 3", "hola", ""]:
            self.assertEqual(wa.local_command(text), "", text)


if __name__ == "__main__":
    unittest.main()
