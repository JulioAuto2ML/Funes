"""Unit tests for speak.py.

Bluetooth, PulseAudio and espeak-ng are stubbed through `speak._run`: what is
under test is the script's own decisions — what text it will say, when it keeps
quiet, and how it gets the Echo's audio sink back when the sink is missing.
Whether the Echo actually plays the sound is checked by ear, not here.
"""

import io
import json
import os
import subprocess
import sys
import unittest
import wave
from contextlib import redirect_stderr, redirect_stdout
from datetime import time as dtime
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import speak  # noqa: E402

MAC = "78:6C:84:CB:44:AF"
SINK = "bluez_sink.78_6C_84_CB_44_AF.a2dp_sink"
SINKS_WITH_ECHO = f"0\talsa_output.pci\tmodule-alsa-card.c\ts16le 2ch 44100Hz\tSUSPENDED\n2\t{SINK}\tmodule-bluez5-device.c\ts16le 2ch 44100Hz\tIDLE\n"
SINKS_WITHOUT = "0\talsa_output.pci\tmodule-alsa-card.c\ts16le 2ch 44100Hz\tSUSPENDED\n"


def wav_bytes(frames=2205, rate=22050):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"\x01\x00" * frames)
    return buf.getvalue()


def cp(stdout="", code=0, out_bytes=None):
    return subprocess.CompletedProcess([], code, stdout=out_bytes if out_bytes is not None else stdout, stderr="")


class Env(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.dict(os.environ, {"FUNES_ECHO_MAC": MAC, "FUNES_SPEAK_QUIET": "off"}, clear=False)
        patcher.start()
        self.addCleanup(patcher.stop)


class FakeSystem:
    """Scripted answers for speak._run, keyed on the program being run."""

    def __init__(self, sink_after_connects=0, sink_present=True):
        self.calls = []
        self.connects = 0
        self.power_cycles = 0
        self.sink_after_connects = sink_after_connects
        self.sink_present = sink_present
        self.played = []

    def sink_visible(self):
        if self.sink_present:
            return True
        return self.connects > self.sink_after_connects

    def __call__(self, argv, timeout, text=True):
        self.calls.append(argv)
        prog = argv[0]
        if prog == "pactl":
            return cp(SINKS_WITH_ECHO if self.sink_visible() else SINKS_WITHOUT)
        if prog == "bluetoothctl":
            if argv[1] == "connect":
                self.connects += 1
            if argv[1] == "power" and argv[2] == "off":
                self.power_cycles += 1
            return cp("")
        if prog == "espeak-ng":
            return cp(out_bytes=wav_bytes())
        if prog == "paplay":
            self.played.append(argv)
            return cp("")
        raise AssertionError(f"unexpected program {argv}")


class TestText(Env):
    def test_refuses_empty_text(self):
        self.assertEqual(speak.perform(["--text", "   "])[0], 2)

    def test_refuses_text_over_the_limit_rather_than_truncating(self):
        code, _ = speak.perform(["--text", "a" * (speak.MAX_CHARS + 1)])
        self.assertEqual(code, 2)

    def test_control_characters_are_dropped_before_synthesis(self):
        fake = FakeSystem()
        with mock.patch.object(speak, "_run", fake), mock.patch.object(speak.time, "sleep"):
            code, _ = speak.perform(["--text", "hola\x00\x1b[31m mundo"])
        self.assertEqual(code, 0)
        said = [c for c in fake.calls if c[0] == "espeak-ng"][0][-1]
        self.assertNotIn("\x00", said)
        self.assertNotIn("\x1b", said)

    def test_text_starting_with_dash_cannot_become_an_espeak_option(self):
        fake = FakeSystem()
        with mock.patch.object(speak, "_run", fake), mock.patch.object(speak.time, "sleep"):
            speak.perform(["--text=--stdout -v en"])
        argv = [c for c in fake.calls if c[0] == "espeak-ng"][0]
        self.assertEqual(argv[-2], "--")


class TestConfig(Env):
    def test_missing_mac_is_an_installation_fault(self):
        with mock.patch.dict(os.environ, {"FUNES_ECHO_MAC": ""}):
            self.assertEqual(speak.perform(["--text", "hola"])[0], 3)

    def test_malformed_mac_is_refused_before_anything_runs(self):
        fake = FakeSystem()
        with mock.patch.dict(os.environ, {"FUNES_ECHO_MAC": "78:6C; rm -rf"}), mock.patch.object(speak, "_run", fake):
            self.assertEqual(speak.perform(["--text", "hola"])[0], 3)
        self.assertEqual(fake.calls, [])


class TestQuietHours(Env):
    def test_window_wrapping_midnight(self):
        spec = "23:00-07:00"
        self.assertTrue(speak.in_quiet_hours(dtime(23, 30), spec))
        self.assertTrue(speak.in_quiet_hours(dtime(3, 0), spec))
        self.assertFalse(speak.in_quiet_hours(dtime(7, 0), spec))
        self.assertFalse(speak.in_quiet_hours(dtime(12, 0), spec))

    def test_window_inside_one_day(self):
        self.assertTrue(speak.in_quiet_hours(dtime(14, 0), "13:00-15:00"))
        self.assertFalse(speak.in_quiet_hours(dtime(16, 0), "13:00-15:00"))

    def test_off_never_blocks(self):
        self.assertFalse(speak.in_quiet_hours(dtime(3, 0), "off"))

    def test_bad_spec_fails_open_to_the_default_window(self):
        self.assertTrue(speak.in_quiet_hours(dtime(3, 0), "nonsense"))

    def test_quiet_hours_refuse_with_exit_5_and_touch_nothing(self):
        fake = FakeSystem()
        with mock.patch.dict(os.environ, {"FUNES_SPEAK_QUIET": "00:00-23:59"}), \
                mock.patch.object(speak, "_run", fake), \
                mock.patch.object(speak, "_now", return_value=dtime(12, 0)):
            code, _ = speak.perform(["--text", "hola"])
        self.assertEqual(code, 5)
        self.assertEqual(fake.calls, [])


class TestSink(Env):
    def test_present_sink_is_used_without_reconnecting(self):
        fake = FakeSystem(sink_present=True)
        with mock.patch.object(speak, "_run", fake), mock.patch.object(speak.time, "sleep"):
            code, result = speak.perform(["--text", "hola"])
        self.assertEqual(code, 0)
        self.assertEqual(fake.connects, 0)
        self.assertFalse(result["reconnected"])
        self.assertIn(f"--device={SINK}", fake.played[0])

    def test_missing_sink_triggers_one_connect(self):
        fake = FakeSystem(sink_present=False, sink_after_connects=0)
        with mock.patch.object(speak, "_run", fake), mock.patch.object(speak.time, "sleep"):
            code, result = speak.perform(["--text", "hola"])
        self.assertEqual(code, 0)
        self.assertEqual(fake.connects, 1)
        self.assertEqual(fake.power_cycles, 0)
        self.assertTrue(result["reconnected"])

    def test_power_cycle_is_the_last_resort_not_the_first(self):
        # The sink only appears after the second connect, i.e. after a power cycle.
        fake = FakeSystem(sink_present=False, sink_after_connects=1)
        with mock.patch.object(speak, "_run", fake), mock.patch.object(speak.time, "sleep"):
            code, result = speak.perform(["--text", "hola"])
        self.assertEqual(code, 0)
        self.assertEqual(fake.power_cycles, 1)
        self.assertTrue(result["reconnected"])

    def test_no_sink_after_recovery_is_exit_4(self):
        fake = FakeSystem(sink_present=False, sink_after_connects=99)
        buf = io.StringIO()
        with mock.patch.object(speak, "_run", fake), mock.patch.object(speak.time, "sleep"), redirect_stderr(buf):
            code, _ = speak.perform(["--text", "hola"])
        self.assertEqual(code, 4)
        self.assertEqual(fake.played, [])
        self.assertEqual(fake.power_cycles, 1)  # tried once, did not loop


class TestSynthesis(Env):
    def test_lead_silence_is_prepended(self):
        out = speak.with_lead_silence(wav_bytes(frames=100, rate=22050), 0.5)
        with wave.open(io.BytesIO(out)) as w:
            self.assertEqual(w.getnframes(), 100 + int(22050 * 0.5))
            self.assertEqual(w.getframerate(), 22050)

    def test_language_picks_the_voice(self):
        fake = FakeSystem()
        with mock.patch.object(speak, "_run", fake), mock.patch.object(speak.time, "sleep"):
            speak.perform(["--text", "hello", "--lang", "en"])
        argv = [c for c in fake.calls if c[0] == "espeak-ng"][0]
        self.assertIn(speak.VOICES["en"], argv)

    def test_espeak_failure_is_a_failure_not_silence(self):
        def broken(argv, timeout, text=True):
            if argv[0] == "espeak-ng":
                return cp(code=1, out_bytes=b"")
            return FakeSystem()(argv, timeout, text)

        buf = io.StringIO()
        with mock.patch.object(speak, "_run", broken), mock.patch.object(speak.time, "sleep"), redirect_stderr(buf):
            code, _ = speak.perform(["--text", "hola"])
        self.assertEqual(code, 1)


class TestMain(Env):
    def test_stdout_is_exactly_one_json_object(self):
        fake = FakeSystem()
        out = io.StringIO()
        with mock.patch.object(speak, "_run", fake), mock.patch.object(speak.time, "sleep"), redirect_stdout(out):
            code = speak.main(["--text", "hola"])
        self.assertEqual(code, 0)
        parsed = json.loads(out.getvalue())
        self.assertTrue(parsed["ok"])
        self.assertEqual(parsed["chars"], 4)


if __name__ == "__main__":
    unittest.main()
