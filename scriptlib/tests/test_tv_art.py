"""Unit tests for tv_art.py.

The TV is stubbed: what is under test is the script's own decisions — which
image paths it will read, what it does when the TV is slow or off, and that a
`show` replaces the previous upload instead of filling the TV's storage one
test image at a time. Whether the Samsung API itself works is checked by hand
against the real set, not here.
"""

import io
import json
import os
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import tv_art  # noqa: E402

JPEG = b"\xff\xd8\xff\xe0" + b"0" * 64


class FakeArt:
    """Just enough of samsungtvws' art() API, with a configurable slow step."""

    def __init__(self, artmode="off", current="SAM-S1", slow_artmode=False):
        self.artmode = artmode
        self.current = current
        self.uploads = []
        self.deleted = []
        self.slow_artmode = slow_artmode
        self._next = 1

    def supported(self):
        return True

    def get_artmode(self):
        return self.artmode

    def set_artmode(self, mode):
        # The real set applies the change and then times out answering.
        self.artmode = mode
        if self.slow_artmode:
            raise TimeoutError("Websocket Time out")

    def get_current(self):
        return {"content_id": self.current}

    def upload(self, data, file_type="JPEG", matte="none"):
        cid = f"MY_F{self._next:04d}"
        self._next += 1
        self.uploads.append((cid, file_type, matte, len(data)))
        return cid

    def select_image(self, cid, show=True):
        self.current = cid

    def delete(self, cid):
        self.deleted.append(cid)

    def available(self, category=None):
        return [{"content_id": c} for c, *_ in self.uploads]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.ws = self.root / "ws"
        self.ws.mkdir()
        self.token = self.root / "tv_token"
        self.env = mock.patch.dict(os.environ, {
            "FUNES_TV_HOST": "192.0.2.10",
            "FUNES_TV_TOKEN_FILE": str(self.token),
        })
        self.env.start()
        self.cwd = os.getcwd()
        os.chdir(self.ws)
        self.art = FakeArt()
        self.connect = mock.patch.object(tv_art, "connect", return_value=self.art)
        self.connect.start()
        mock.patch.object(tv_art, "SETTLE_SECONDS", 0).start()
        mock.patch.object(tv_art, "WAKE_SECONDS", 0).start()
        mock.patch.object(tv_art, "BUDGET_SECONDS", 0.2).start()

    def tearDown(self):
        mock.patch.stopall()
        os.chdir(self.cwd)
        self.tmp.cleanup()

    def state_file(self):
        return Path(str(self.token) + ".state.json")

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = tv_art.main(list(argv))
        return code, out.getvalue(), err.getvalue()


class TestArguments(Base):
    def test_unknown_action_refused(self):
        code, out, err = self.run_main("--action=reboot")
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("action", err)

    def test_missing_config_is_an_installation_fault(self):
        del os.environ["FUNES_TV_HOST"]
        code, _, err = self.run_main("--action=status")
        self.assertEqual(code, 3)
        self.assertIn("FUNES_TV_HOST", err)

    def test_show_needs_an_image(self):
        code, _, err = self.run_main("--action=show")
        self.assertEqual(code, 2)
        self.assertIn("--image", err)


class TestImagePath(Base):
    """The script runs with Funes' own permissions, outside fs_guard, so it
    confines the image path itself: the caller's workspace and nothing else."""

    def refused(self, image):
        code, out, err = self.run_main("--action=show", f"--image={image}")
        self.assertEqual(code, 2, err)
        self.assertEqual(self.art.uploads, [])
        return err

    def test_absolute_path_refused(self):
        self.refused("/etc/hostname")

    def test_parent_escape_refused(self):
        (self.root / "outside.jpg").write_bytes(JPEG)
        self.refused("../outside.jpg")

    def test_symlink_out_of_workspace_refused(self):
        (self.root / "outside.jpg").write_bytes(JPEG)
        (self.ws / "link.jpg").symlink_to(self.root / "outside.jpg")
        self.refused("link.jpg")

    def test_wrong_extension_refused(self):
        (self.ws / "notes.txt").write_text("hi")
        self.assertIn("jpg", self.refused("notes.txt"))

    def test_missing_file_refused(self):
        self.assertIn("not found", self.refused("ghost.jpg"))

    def test_png_is_uploaded_as_png(self):
        (self.ws / "sub").mkdir()
        (self.ws / "sub" / "a.png").write_bytes(b"\x89PNG" + b"0" * 32)
        code, _, err = self.run_main("--action=show", "--image=sub/a.png")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.art.uploads[0][1], "PNG")


class TestShow(Base):
    def test_show_uploads_selects_and_turns_art_mode_on(self):
        (self.ws / "a.jpg").write_bytes(JPEG)
        code, out, err = self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(code, 0, err)
        result = json.loads(out)
        self.assertEqual(result["content_id"], "MY_F0001")
        self.assertEqual(result["artmode"], "on")
        self.assertEqual(result["replaced"], [])
        self.assertEqual(self.art.uploads[0][2], "none")

    def test_slow_art_mode_reply_is_verified_not_trusted(self):
        # The set times out answering set_artmode but applies it: the result
        # comes from reading the state back, not from the reply.
        self.art.slow_artmode = True
        (self.ws / "a.jpg").write_bytes(JPEG)
        code, out, err = self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["artmode"], "on")

    def test_second_show_replaces_the_first_upload(self):
        (self.ws / "a.jpg").write_bytes(JPEG)
        self.run_main("--action=show", "--image=a.jpg")
        code, out, _ = self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["replaced"], ["MY_F0001"])
        self.assertEqual(self.art.deleted, ["MY_F0001"])

    def test_keep_leaves_the_previous_upload(self):
        (self.ws / "a.jpg").write_bytes(JPEG)
        self.run_main("--action=show", "--image=a.jpg")
        code, out, _ = self.run_main("--action=show", "--image=a.jpg", "--keep")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["replaced"], [])
        self.assertEqual(self.art.deleted, [])

    def test_never_deletes_what_it_did_not_upload(self):
        # A state file naming a Samsung Art Store id is not ours to delete.
        self.state_file().write_text(json.dumps({"uploaded": ["SAM-S1"]}))
        (self.ws / "a.jpg").write_bytes(JPEG)
        code, out, _ = self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(code, 0)
        self.assertEqual(self.art.deleted, [])

    def test_upload_is_recorded_before_anything_can_go_wrong(self):
        # Seen against the real set: a run killed after the upload but before
        # the end left an image nobody would ever clean up. The id is on disk
        # the moment the TV returns it, so the next show removes it.
        def select_dies(cid, show=True):
            raise KeyboardInterrupt   # stands in for the manifest's kill
        self.art.select_image = select_dies
        (self.ws / "a.jpg").write_bytes(JPEG)
        with self.assertRaises(KeyboardInterrupt):
            self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(json.loads(self.state_file().read_text())["uploaded"], ["MY_F0001"])

        del self.art.select_image          # back to the working fake
        code, out, _ = self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(code, 0)
        self.assertEqual(self.art.deleted, ["MY_F0001"])
        self.assertEqual(json.loads(out)["replaced"], ["MY_F0001"])
        self.assertEqual(json.loads(self.state_file().read_text())["uploaded"], ["MY_F0002"])

    def test_kept_image_is_never_deleted_later(self):
        (self.ws / "a.jpg").write_bytes(JPEG)
        self.run_main("--action=show", "--image=a.jpg")            # MY_F0001
        self.run_main("--action=show", "--image=a.jpg", "--keep")  # MY_F0002, keeps 1
        self.run_main("--action=show", "--image=a.jpg")            # MY_F0003
        self.assertEqual(self.art.deleted, ["MY_F0002"])

    def test_art_mode_already_on_is_not_switched_again(self):
        # Each extra call is one more chance for the set to time out.
        self.art.artmode = "on"
        self.art.set_artmode = mock.Mock(side_effect=AssertionError("not needed"))
        (self.ws / "a.jpg").write_bytes(JPEG)
        code, _, err = self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(code, 0, err)
        self.art.set_artmode.assert_not_called()

    def test_finishes_inside_its_budget(self):
        # A set that never confirms must not run into the manifest's kill:
        # the script gives up on its own, with a sentence, before then.
        self.art.select_image = lambda cid, show=True: None
        clock = iter(range(0, 10_000, 7))
        with mock.patch.object(tv_art.time, "monotonic", lambda: next(clock)):
            (self.ws / "a.jpg").write_bytes(JPEG)
            code, _, err = self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(code, 1)
        self.assertIn("did not select", err)

    def test_selection_that_did_not_take_is_a_failure(self):
        self.art.select_image = lambda cid, show=True: None   # silently ignored
        (self.ws / "a.jpg").write_bytes(JPEG)
        code, out, err = self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(code, 1)
        self.assertEqual(out, "")
        self.assertIn("MY_F0001", err)


class TestOtherActions(Base):
    def test_status(self):
        code, out, _ = self.run_main("--action=status")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out), {
            "ok": True, "action": "status", "artmode": "off",
            "content_id": "SAM-S1", "replaced": [], "woke": False})

    def test_artmode_off(self):
        self.art.artmode = "on"
        code, out, _ = self.run_main("--action=artmode_off")
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["artmode"], "off")

    def test_tv_unreachable_says_so(self):
        tv_art.connect.side_effect = ConnectionRefusedError("refused")
        code, out, err = self.run_main("--action=status")
        self.assertEqual(code, tv_art.EXIT_UNREACHABLE)
        self.assertEqual(out, "")
        self.assertIn("off or not on the network", err)

    def test_tv_unreachable_on_first_call_says_so(self):
        # samsungtvws connects lazily: an unplugged set fails on the first
        # call, not in connect(). Seen against the real TV: EHOSTUNREACH.
        def gone():
            raise OSError(113, "No route to host")
        self.art.get_artmode = gone
        code, out, err = self.run_main("--action=status")
        self.assertEqual(code, tv_art.EXIT_UNREACHABLE)
        self.assertEqual(out, "")
        self.assertIn("off or not on the network", err)

    def test_asleep_tv_is_woken_when_its_mac_is_known(self):
        # The Frame drops off the network when Art Mode blanks the screen.
        # With FUNES_TV_MAC set, an unreachable set gets a Wake-on-LAN packet
        # and one more try.
        os.environ["FUNES_TV_MAC"] = "c8:12:0b:1c:91:7a"
        calls = {"n": 0}
        real = self.art.get_artmode
        def asleep_once():
            calls["n"] += 1
            if calls["n"] == 1:
                raise OSError(113, "No route to host")
            return real()
        self.art.get_artmode = asleep_once
        with mock.patch.object(tv_art, "wake") as wake:
            code, out, err = self.run_main("--action=status")
        self.assertEqual(code, 0, err)
        # The subnet broadcast of FUNES_TV_HOST, not 255.255.255.255: on yoda the
        # default route is another link, and a limited broadcast left by it.
        wake.assert_called_once_with("c8:12:0b:1c:91:7a", "192.0.2.255")
        self.assertTrue(json.loads(out)["woke"])

    def test_no_wake_leaves_a_sleeping_tv_alone(self):
        # Scheduled runs pass --no-wake: an empty living room is not a reason
        # to switch the set on every hour.
        os.environ["FUNES_TV_MAC"] = "c8:12:0b:1c:91:7a"
        def gone():
            raise OSError(113, "No route to host")
        self.art.get_artmode = gone
        with mock.patch.object(tv_art, "wake") as wake:
            code, out, err = self.run_main("--action=status", "--no-wake")
        self.assertEqual(code, tv_art.EXIT_UNREACHABLE)
        wake.assert_not_called()
        self.assertIn("off or not on the network", err)

    def test_perform_returns_the_result_instead_of_printing(self):
        # tv_weather_art reuses tv_art in-process and prints its own JSON.
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(io.StringIO()):
            code, result = tv_art.perform(["--action=status"])
        self.assertEqual(code, 0)
        self.assertEqual(result["content_id"], "SAM-S1")
        self.assertEqual(out.getvalue(), "")

    def test_magic_packet_shape(self):
        sent = []
        class Sock:
            def __init__(self, *a): pass
            def setsockopt(self, *a): pass
            def sendto(self, data, addr): sent.append((data, addr))
            def close(self): pass
        with mock.patch.object(tv_art.socket, "socket", Sock):
            tv_art.wake("c8:12:0b:1c:91:7a", "192.0.2.255")
        data, addr = sent[0]
        self.assertEqual(addr, ("192.0.2.255", 9))
        self.assertEqual(len(data), 102)
        self.assertEqual(data[:6], b"\xff" * 6)
        self.assertEqual(data[6:12], bytes.fromhex("c8120b1c917a"))
        self.assertEqual(addr[1], 9)

    def test_broadcast_address(self):
        self.assertEqual(tv_art.broadcast_for("192.168.1.55"), "192.168.1.255")
        os.environ["FUNES_TV_BROADCAST"] = "10.0.255.255"
        self.assertEqual(tv_art.broadcast_for("10.0.3.4"), "10.0.255.255")

    def test_slow_reply_is_not_reported_as_tv_off(self):
        # TimeoutError is an OSError too; it must not read as "unplugged".
        self.art.upload = lambda *a, **k: (_ for _ in ()).throw(TimeoutError("slow"))
        (self.ws / "a.jpg").write_bytes(JPEG)
        code, _, err = self.run_main("--action=show", "--image=a.jpg")
        self.assertEqual(code, 1)
        self.assertNotIn("off or not on the network", err)
        self.assertIn("TimeoutError", err)


if __name__ == "__main__":
    unittest.main()
