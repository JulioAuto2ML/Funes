#!/usr/bin/env python3
"""Restart one of the user services the manifest lists, and report its state.

--unit=NAME   one of the manifest's choices (the runtime refuses anything else
              before this runs; checked again here)

Not `funes` itself: the run asking would be killed by its own restart, and a
deploy is `git pull` + rebuild, not this.
"""
import subprocess
import sys
import time

ALLOWED = {"whatsapp-bridge", "whatsapp-bridge-funes", "whatsapp-autoresponder", "yoda-monitor"}


def parse_args(argv):
    args = {}
    for token in argv:
        if not token.startswith("--"):
            raise SystemExit(f"unexpected argument: {token}")
        key, sep, value = token[2:].partition("=")
        args[key] = value if sep else True
    return args


def state(unit):
    r = subprocess.run(["systemctl", "--user", "show", unit, "-p",
                        "ActiveState,SubState,NRestarts,ActiveEnterTimestamp"],
                       capture_output=True, text=True, timeout=10)
    return dict(l.split("=", 1) for l in r.stdout.splitlines() if "=" in l)


def main():
    unit = parse_args(sys.argv[1:]).get("unit", "")
    if unit not in ALLOWED:
        raise SystemExit(f"{unit or '(none)'} is not a unit this script restarts: {', '.join(sorted(ALLOWED))}")
    name = unit + ".service"
    before = state(name)
    r = subprocess.run(["systemctl", "--user", "restart", name], capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        raise SystemExit(f"restart failed ({r.returncode}): {r.stderr.strip()}")
    # A unit that dies on start is "active" for a moment first.
    time.sleep(3)
    after = state(name)
    print(f"{name}: was {before.get('ActiveState')}/{before.get('SubState')}, "
          f"now {after.get('ActiveState')}/{after.get('SubState')} since {after.get('ActiveEnterTimestamp')}")
    if after.get("ActiveState") != "active":
        sys.exit(1)


if __name__ == "__main__":
    main()
