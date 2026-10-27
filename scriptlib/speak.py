#!/usr/bin/env python3
"""speak — say a sentence aloud on the living-room Echo, over Bluetooth.

Run by Funes through `run_script` (see speak.yaml); stdout is exactly one JSON
object, everything else goes to stderr. Standard library only: it shells out to
`espeak-ng` (voice), `paplay` (PulseAudio playback) and `bluetoothctl`.

    --text=<sentence> [--lang=es|en] [--speed=150]

What this script decides for itself rather than trusting the caller:

- **What it will say.** Control characters are dropped, empty text and text over
  MAX_CHARS are refused (not truncated: a sentence cut in the middle reads as a
  different sentence). The text goes after `--` so it can never become an
  espeak-ng option.
- **When it keeps quiet.** `FUNES_SPEAK_QUIET` (default 23:00-07:00, local
  time, `off` disables) refuses with exit 5. A scheduled job firing at 03:00
  must not wake the house, and there is no "urgent" flag for a model to reach
  for.
- **Whether the Echo is reachable.** The Echo's audio exists for PulseAudio
  only while the Bluetooth link is up. A missing sink gets one `connect`; if the
  sink is still missing, one controller power cycle and another `connect`. The
  power cycle is the fix for a state hit on 2026-10-22: the adapter held a
  phantom ACL link to the Echo, BlueZ did not know about it, and every
  pair/connect came back "Already Connected" until the adapter was reset. It is
  the last resort, tried once, never looped.
- **The first half-second.** An A2DP stream takes a moment to open and the
  Echo swallows the start of what it is given, so the audio is prefixed with
  silence rather than losing the first word.

Exit codes: 0 spoken, 1 synthesis or playback failed, 2 bad arguments,
3 not configured (an installation fault, not something a retry fixes),
4 the Echo could not be reached, 5 quiet hours.

Config, from the manifest's env (config/funes.local):
    FUNES_ECHO_MAC       the Echo's Bluetooth *audio* address (the one whose
                         `bluetoothctl info` lists "Audio Sink")
    FUNES_SPEAK_QUIET    optional; "HH:MM-HH:MM" or "off"
"""

import argparse
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import wave
from datetime import datetime
from datetime import time as dtime
from pathlib import Path

MAX_CHARS = 600
VOICES = {"es": "es-419", "en": "en-us"}
DEFAULT_QUIET = "23:00-07:00"
LEAD_SILENCE_S = 0.8
SINK_POLLS = 8  # one second apart, after each connect
MAC_RE = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")
CONTROL_RE = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")


class SpeakError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def _run(argv, timeout, text=True):
    """Run one program; a timeout or a missing program is a result, not a crash."""
    try:
        return subprocess.run(argv, capture_output=True, text=text, timeout=timeout)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(argv, 124, stdout=b"" if not text else "", stderr="timed out")
    except FileNotFoundError:
        return subprocess.CompletedProcess(argv, 127, stdout=b"" if not text else "", stderr=f"{argv[0]}: not installed")


def _now():
    return datetime.now().time()


def _parse_window(spec):
    m = re.fullmatch(r"(\d{1,2}):(\d{2})-(\d{1,2}):(\d{2})", spec.strip())
    if not m:
        return None
    h1, m1, h2, m2 = (int(g) for g in m.groups())
    if h1 > 23 or h2 > 23 or m1 > 59 or m2 > 59:
        return None
    return dtime(h1, m1), dtime(h2, m2)


def in_quiet_hours(now, spec):
    """True when `now` falls in the window. An unreadable spec uses the default
    window: a typo in the config must make the house quieter, not louder."""
    if spec.strip().lower() == "off":
        return False
    window = _parse_window(spec) or _parse_window(DEFAULT_QUIET)
    start, end = window
    if start <= end:
        return start <= now < end
    return now >= start or now < end


def clean_text(raw):
    text = CONTROL_RE.sub("", raw).strip()
    if not text:
        raise SpeakError(2, "speak: --text is empty")
    if len(text) > MAX_CHARS:
        raise SpeakError(2, f"speak: text is {len(text)} characters, the limit is {MAX_CHARS}; say it in fewer words")
    return text


def echo_mac():
    mac = os.environ.get("FUNES_ECHO_MAC", "").strip()
    if not mac:
        raise SpeakError(3, "speak: FUNES_ECHO_MAC is not set")
    if not MAC_RE.match(mac):
        raise SpeakError(3, "speak: FUNES_ECHO_MAC is not a Bluetooth address")
    return mac.upper()


def find_sink(mac):
    prefix = "bluez_sink." + mac.replace(":", "_")
    out = _run(["pactl", "list", "short", "sinks"], timeout=10)
    if out.returncode != 0:
        return None
    for line in out.stdout.splitlines():
        cols = line.split("\t")
        if len(cols) > 1 and cols[1].startswith(prefix):
            return cols[1]
    return None


def _connect_and_wait(mac):
    _run(["bluetoothctl", "connect", mac], timeout=25)
    for _ in range(SINK_POLLS):
        sink = find_sink(mac)
        if sink:
            return sink
        time.sleep(1)
    return None


def ensure_sink(mac):
    """(sink name, whether a reconnect was needed). Raises SpeakError(4)."""
    sink = find_sink(mac)
    if sink:
        return sink, False
    sink = _connect_and_wait(mac)
    if sink:
        return sink, True
    print("speak: no audio sink after connect; resetting the Bluetooth adapter once", file=sys.stderr)
    _run(["bluetoothctl", "power", "off"], timeout=15)
    time.sleep(3)
    _run(["bluetoothctl", "power", "on"], timeout=15)
    time.sleep(3)
    sink = _connect_and_wait(mac)
    if sink:
        return sink, True
    raise SpeakError(4, "speak: the Echo is not reachable over Bluetooth (is it on, in range, and paired with this host?)")


def with_lead_silence(wav_data, seconds):
    with wave.open(io.BytesIO(wav_data)) as src:
        params = src.getparams()
        frames = src.readframes(src.getnframes())
    silence = b"\x00" * (int(params.framerate * seconds) * params.nchannels * params.sampwidth)
    out = io.BytesIO()
    with wave.open(out, "wb") as dst:
        dst.setparams(params)
        dst.writeframes(silence + frames)
    return out.getvalue()


def synthesize(text, lang, speed):
    argv = ["espeak-ng", "-v", VOICES[lang], "-s", str(speed), "--stdout", "--", text]
    out = _run(argv, timeout=20, text=False)
    if out.returncode != 0 or not out.stdout:
        why = out.stderr if isinstance(out.stderr, str) else out.stderr.decode(errors="replace")
        raise SpeakError(1, f"speak: espeak-ng failed ({out.returncode}) {why.strip()}")
    return with_lead_silence(out.stdout, LEAD_SILENCE_S)


def play(sink, wav_data):
    with wave.open(io.BytesIO(wav_data)) as w:
        seconds = w.getnframes() / float(w.getframerate())
    with tempfile.TemporaryDirectory(prefix="speak-") as tmp:
        path = Path(tmp) / "say.wav"
        path.write_bytes(wav_data)
        out = _run(["paplay", f"--device={sink}", str(path)], timeout=15 + seconds)
    if out.returncode != 0:
        raise SpeakError(1, f"speak: paplay failed ({out.returncode}) {out.stderr.strip()}")


def parse_args(argv):
    p = argparse.ArgumentParser(prog="speak", description="Say a sentence on the Echo.")
    p.add_argument("--text", required=True)
    p.add_argument("--lang", choices=sorted(VOICES), default="es")
    p.add_argument("--speed", type=int, default=150)
    return p.parse_args(argv)


def perform(argv):
    """The whole run, returning (exit code, result). main() prints it."""
    try:
        try:
            args = parse_args(argv)
        except SystemExit:
            return 2, None
        if not 80 <= args.speed <= 220:
            raise SpeakError(2, "speak: --speed must be between 80 and 220")
        text = clean_text(args.text)
        mac = echo_mac()
        if in_quiet_hours(_now(), os.environ.get("FUNES_SPEAK_QUIET", DEFAULT_QUIET)):
            raise SpeakError(5, "speak: quiet hours; nothing was said")
        sink, reconnected = ensure_sink(mac)
        play(sink, synthesize(text, args.lang, args.speed))
        return 0, {"ok": True, "chars": len(text), "lang": args.lang, "sink": sink, "reconnected": reconnected}
    except SpeakError as err:
        print(err, file=sys.stderr)
        return err.code, None


def main(argv=None):
    os.environ.setdefault("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")  # where PulseAudio's socket lives
    code, result = perform(sys.argv[1:] if argv is None else argv)
    if result is not None:
        print(json.dumps(result))
    return code


if __name__ == "__main__":
    sys.exit(main())
