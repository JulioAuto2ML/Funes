#!/usr/bin/env python3
"""tv_art — put an image on the Samsung Frame's Art Mode, or read what is there.

Run by Funes through `run_script` (see tv_art.yaml); stdout is exactly one JSON
object, everything else goes to stderr.

    --action=status                     what Art Mode is showing
    --action=show --image=<path> [--keep]
                                        upload, select, switch to Art Mode
    --action=artmode_on | artmode_off

What this script decides for itself rather than trusting the caller or the TV:

- **Which file it reads.** A script runs with Funes' own permissions, outside
  fs_guard, so `--image` is confined here: relative, inside the working
  directory (the caller's workspace), after resolving symlinks.
- **Whether a change happened.** The set routinely applies a command and then
  times out answering it. Every change is read back, and the result reports
  the state read back, not the reply.
- **What it deletes.** Every id it uploads is written to a state file beside
  the token *the moment the TV returns it* — a run killed halfway must not
  leave an image nobody will ever clean up. A successful `show` deletes the
  earlier ones (unless `--keep`, which retires them from the list instead).
  It never deletes an id it did not upload: the Art Store pieces and the
  owner's own photos are not its to touch.
- **How long it takes.** The manifest kills the process at 120 s. The script
  keeps its own budget below that, so a set that never confirms ends in a
  sentence on stderr rather than a kill with nothing said.
- **Whether the set is asleep.** The Frame drops off the network when Art Mode
  blanks the screen. With FUNES_TV_MAC set, an unreachable set gets a
  Wake-on-LAN packet and one more try.

Exit codes: 0 done, 1 the TV failed or refused, 2 bad arguments,
3 not configured (an installation fault, not something a retry fixes),
4 the TV is off or not on the network (and was not, or could not be, woken).

`--no-wake` skips Wake-on-LAN: scheduled runs use it, since an empty living
room is not a reason to switch the set on. `perform(argv)` is the same run
returning (exit code, result) for callers in this directory (tv_weather_art).

Config, from the manifest's env (config/funes.local):
    FUNES_TV_HOST        the TV's address
    FUNES_TV_TOKEN_FILE  where the pairing token lives (outside any workspace)
    FUNES_TV_MAC         optional; enables waking a sleeping set
    FUNES_TV_BROADCAST   optional; default: the TV's /24 broadcast (x.y.z.255)
"""

import errno
import json
import os
import socket
import sys
import time
from pathlib import Path

ACTIONS = ("status", "show", "artmode_on", "artmode_off")
TYPES = {".jpg": "JPEG", ".jpeg": "JPEG", ".png": "PNG"}
MAX_BYTES = 20 * 1024 * 1024
TIMEOUT = 20           # per websocket call; the set is slow, not dead
BUDGET_SECONDS = 100   # under the manifest's 120 s kill
SETTLE_SECONDS = 2     # between a change and reading it back
WAKE_SECONDS = 20      # how long a woken set gets to come back
OUR_PREFIX = "MY_"     # ids of uploaded photos; Art Store ids start SAM-
UNREACHABLE = {errno.EHOSTUNREACH, errno.ECONNREFUSED, errno.ENETUNREACH, errno.EHOSTDOWN}


class Refusal(Exception):
    """Bad arguments: exit 2."""


class NotConfigured(Exception):
    """Missing configuration: exit 3."""


class Unreachable(Exception):
    """The set is off or not on the network."""


def log(msg):
    print(f"tv_art: {msg}", file=sys.stderr, flush=True)


# ── arguments and configuration ──────────────────────────────────────────────

def parse(argv):
    args = {"keep": False, "no_wake": False}
    for token in argv:
        if not token.startswith("--"):
            raise Refusal(f"unexpected argument {token!r}")
        key, sep, value = token[2:].partition("=")
        if key in ("keep", "no-wake") and not sep:
            args[key.replace("-", "_")] = True
        elif key in ("action", "image") and sep:
            args[key] = value
        else:
            raise Refusal(f"unknown parameter --{key}")
    if args.get("action") not in ACTIONS:
        raise Refusal(f"--action must be one of {', '.join(ACTIONS)}")
    if args["action"] == "show" and not args.get("image"):
        raise Refusal("--action=show needs --image=<path in your workspace>")
    return args


def config():
    host = os.environ.get("FUNES_TV_HOST", "").strip()
    token = os.environ.get("FUNES_TV_TOKEN_FILE", "").strip()
    missing = [n for n, v in (("FUNES_TV_HOST", host), ("FUNES_TV_TOKEN_FILE", token)) if not v]
    if missing:
        raise NotConfigured(f"{' and '.join(missing)} not set — add to config/funes.local")
    return host, Path(token), os.environ.get("FUNES_TV_MAC", "").strip()


def resolve_image(raw):
    """The image, confined to the working directory. Returns (path, file_type)."""
    if os.path.isabs(raw):
        raise Refusal("--image must be a path relative to your workspace, not absolute")
    root = Path.cwd().resolve()
    path = (root / raw).resolve()
    if root != path and root not in path.parents:
        raise Refusal("--image must stay inside your workspace")
    file_type = TYPES.get(path.suffix.lower())
    if not file_type:
        raise Refusal("--image must be a .jpg, .jpeg or .png file")
    if not path.is_file():
        raise Refusal(f"--image not found in your workspace: {raw}")
    if path.stat().st_size > MAX_BYTES:
        raise Refusal("--image is over 20 MB")
    return path, file_type


# ── the TV ───────────────────────────────────────────────────────────────────

def connect(host, token_file):
    """The TV's art channel. Imported here so the tests need no samsungtvws."""
    import urllib3
    from samsungtvws import SamsungTVWS
    urllib3.disable_warnings()   # the set's certificate is self-signed
    token_file.parent.mkdir(parents=True, exist_ok=True)
    tv = SamsungTVWS(host=host, port=8002, token_file=str(token_file),
                     name="Funes", timeout=TIMEOUT)
    return tv.art()


def broadcast_for(host):
    """Where the magic packet goes: FUNES_TV_BROADCAST, else the TV's /24.

    Not 255.255.255.255: a limited broadcast leaves by the default route, and
    on yoda that is a different link from the TV's (seen: the packet went to
    the wired link to luke and the set never woke)."""
    configured = os.environ.get("FUNES_TV_BROADCAST", "").strip()
    if configured:
        return configured
    return ".".join(host.split(".")[:3] + ["255"])


def wake(mac, broadcast):
    """Wake-on-LAN: six 0xFF then the MAC sixteen times, to port 9."""
    raw = bytes.fromhex(mac.replace(":", "").replace("-", ""))
    packet = b"\xff" * 6 + raw * 16
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.sendto(packet, (broadcast, 9))
    finally:
        s.close()


def is_unreachable(e):
    return isinstance(e, OSError) and not isinstance(e, TimeoutError) \
        and (e.errno in UNREACHABLE or isinstance(e, ConnectionError))


class Session:
    """One run against the set: a deadline, and read-back instead of trust."""

    def __init__(self, art):
        self.art = art
        self.deadline = time.monotonic() + BUDGET_SECONDS

    def left(self):
        return self.deadline - time.monotonic()

    def attempt(self, what, fn):
        """One call whose reply may not come; the change is checked by reading back."""
        try:
            return fn()
        except Exception as e:  # samsungtvws raises several unrelated types
            if is_unreachable(e):
                raise
            log(f"{what}: {type(e).__name__}: {e} (checking whether it applied)")
            return None

    def read_back(self, check):
        while self.left() > 0:
            time.sleep(SETTLE_SECONDS)
            if self.attempt("read back", check):
                return True
        return False

    def ensure_artmode(self, mode):
        if self.attempt("read art mode", self.art.get_artmode) == mode:
            return
        self.attempt(f"set art mode {mode}", lambda: self.art.set_artmode(mode))
        if not self.read_back(lambda: self.art.get_artmode() == mode):
            raise RuntimeError(f"the TV did not switch Art Mode {mode} within "
                               f"{BUDGET_SECONDS} s")


# ── uploads this script owns ─────────────────────────────────────────────────

def state_path(token_file):
    return Path(str(token_file) + ".state.json")


def load_uploaded(token_file):
    try:
        ids = json.loads(state_path(token_file).read_text()).get("uploaded", [])
        return [i for i in ids if isinstance(i, str)]
    except (OSError, ValueError, AttributeError):
        return []


def save_uploaded(token_file, ids):
    p = state_path(token_file)
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps({"uploaded": ids}))
    tmp.replace(p)


def show(session, token_file, image, file_type, keep):
    art = session.art
    log(f"uploading {image.name} ({image.stat().st_size} bytes)")
    cid = art.upload(image.read_bytes(), file_type=file_type, matte="none")
    if not cid:
        raise RuntimeError("the TV accepted the upload but returned no content id")
    earlier = [i for i in load_uploaded(token_file) if i != cid]
    save_uploaded(token_file, earlier + [cid])      # before anything else can fail
    log(f"uploaded as {cid}")

    session.attempt(f"select {cid}", lambda: art.select_image(cid, show=True))
    if not session.read_back(lambda: art.get_current().get("content_id") == cid):
        raise RuntimeError(f"uploaded {cid} but the TV did not select it within "
                           f"{BUDGET_SECONDS} s")
    session.ensure_artmode("on")

    if keep:
        save_uploaded(token_file, [cid])            # earlier ones retire, undeleted
        return []
    replaced, remaining = [], []
    for old in earlier:
        if not old.startswith(OUR_PREFIX):
            continue                                # not ours, whatever the file says
        if session.attempt(f"delete {old}", lambda: art.delete(old) or True):
            replaced.append(old)
        else:
            remaining.append(old)                   # try again next time
    save_uploaded(token_file, remaining + [cid])
    return replaced


def run(session, args, token_file, image):
    replaced = []
    if args["action"] == "show":
        replaced = show(session, token_file, *image, args["keep"])
    elif args["action"] == "artmode_on":
        session.ensure_artmode("on")
    elif args["action"] == "artmode_off":
        session.ensure_artmode("off")
    return {
        "ok": True,
        "action": args["action"],
        "artmode": session.art.get_artmode(),
        "content_id": session.art.get_current().get("content_id"),
        "replaced": replaced,
    }


EXIT_UNREACHABLE = 4


def perform(argv):
    """One run. Returns (exit code, result dict or None); logs to stderr."""
    try:
        args = parse(argv)
        host, token_file, mac = config()
        image = resolve_image(args["image"]) if args["action"] == "show" else None
    except Refusal as e:
        log(str(e))
        return 2, None
    except NotConfigured as e:
        log(f"not configured: {e}")
        return 3, None
    if args["no_wake"]:
        mac = ""

    woke = False
    try:
        try:
            result = run(Session(connect(host, token_file)), args, token_file, image)
        except Exception as e:
            if not (is_unreachable(e) and mac):
                raise
            target = broadcast_for(host)
            log(f"unreachable ({e}); sending Wake-on-LAN to {mac} via {target}")
            wake(mac, target)
            woke = True
            time.sleep(WAKE_SECONDS)
            result = run(Session(connect(host, token_file)), args, token_file, image)
    except Exception as e:
        if is_unreachable(e):
            hint = "" if mac or args["no_wake"] else " (set FUNES_TV_MAC to let tv_art wake it)"
            log(f"the TV is off or not on the network ({host}): {e}{hint}")
            return EXIT_UNREACHABLE, None
        log(f"failed: {type(e).__name__}: {e}")
        return 1, None

    result["woke"] = woke
    return 0, result


def main(argv):
    code, result = perform(argv)
    if result is not None:
        print(json.dumps(result))
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
