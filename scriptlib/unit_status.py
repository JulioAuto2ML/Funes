#!/usr/bin/env python3
"""State of yoda's user services (systemctl --user), read-only.

With no argument: every user service, its state, and the failed ones.
With --unit=NAME: that unit's state, result, restarts, start time and memory.

Arguments arrive as `--name=value` tokens (scriptlib/README.md). A unit name
is checked against systemd's own alphabet before it reaches argv — there is
no shell, but a name starting with '-' would still be read as an option.
"""
import re
import subprocess
import sys

UNIT_RE = re.compile(r"^[A-Za-z0-9@._\-]+$")
FIELDS = ("Id", "Description", "LoadState", "ActiveState", "SubState", "Result",
          "NRestarts", "ActiveEnterTimestamp", "InactiveEnterTimestamp",
          "ExecMainStatus", "MemoryCurrent", "MemoryPeak", "CPUUsageNSec")


def parse_args(argv):
    args = {}
    for token in argv:
        if not token.startswith("--"):
            raise SystemExit(f"unexpected argument: {token}")
        key, sep, value = token[2:].partition("=")
        args[key] = value if sep else True
    return args


def systemctl(*argv):
    r = subprocess.run(["systemctl", "--user", "--no-pager", *argv],
                       capture_output=True, text=True, timeout=15)
    if r.returncode not in (0, 3):   # 3: "not active", still an answer
        raise SystemExit(f"systemctl {' '.join(argv)} failed ({r.returncode}): {r.stderr.strip()}")
    return r.stdout


def human_bytes(v):
    try:
        n = int(v)
    except ValueError:
        return v
    if n >= 2**63 - 1 or n < 0:      # "[not set]" comes back as UINT64_MAX
        return "-"
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024:
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TiB"


def one_unit(name):
    if not UNIT_RE.match(name) or name.startswith("-"):
        raise SystemExit(f"not a unit name: {name}")
    if "." not in name:
        name += ".service"
    out = systemctl("show", name, "-p", ",".join(FIELDS))
    props = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    if props.get("LoadState") == "not-found":
        print(f"{name}: no such user unit")
        return
    for key in ("MemoryCurrent", "MemoryPeak"):
        if key in props:
            props[key] = human_bytes(props[key])
    if props.get("CPUUsageNSec", "").isdigit():
        props["CPUUsageNSec"] = f"{int(props['CPUUsageNSec']) / 1e9:.1f} s"
    for key in FIELDS:
        if key in props:
            print(f"{key}: {props[key]}")


def all_units():
    print("User services:")
    print(systemctl("list-units", "--type=service", "--all", "--plain", "--no-legend").rstrip()
          or "  (none)")
    failed = systemctl("list-units", "--failed", "--plain", "--no-legend").strip()
    print("\nFailed units: " + ("none" if not failed else "\n" + failed))


def main():
    args = parse_args(sys.argv[1:])
    unit = args.get("unit")
    if unit:
        one_unit(unit)
    else:
        all_units()


if __name__ == "__main__":
    main()
