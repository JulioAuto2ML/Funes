#!/usr/bin/env python3
"""Which directories take the most space, read-only (du -x).

--path=DIR    where to start; absolute, default /
--depth=N     how many levels below it; 1..3, default 1
--top=N       how many to list, largest first; 1..40, default 15

Stays on the filesystem it starts on (-x). Directories this user cannot read
are counted as unreadable, not guessed at; root-owned parts of /var and the
like come out smaller than they are, and the output says so.
"""
import os
import subprocess
import sys

PATH_OK = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-@+")


def parse_args(argv):
    args = {}
    for token in argv:
        if not token.startswith("--"):
            raise SystemExit(f"unexpected argument: {token}")
        key, sep, value = token[2:].partition("=")
        args[key] = value if sep else True
    return args


def bounded(args, key, default, lo, hi):
    try:
        n = int(float(args.get(key, default)))
    except ValueError:
        raise SystemExit(f"{key} must be a number")
    return max(lo, min(n, hi))


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024


def main():
    args = parse_args(sys.argv[1:])
    path = args.get("path") or "/"
    if not isinstance(path, str) or not path.startswith("/") or ".." in path.split("/") \
            or not set(path) <= PATH_OK:
        raise SystemExit(f"path must be an absolute directory: {path}")
    if not os.path.isdir(path):
        raise SystemExit(f"no such directory: {path}")
    depth = bounded(args, "depth", 1, 1, 3)
    top = bounded(args, "top", 15, 1, 40)

    r = subprocess.run(["du", "-x", "-B1", f"--max-depth={depth}", "--", path],
                       capture_output=True, text=True, timeout=280)
    rows = []
    for line in r.stdout.splitlines():
        size, _, name = line.partition("\t")
        if size.isdigit() and name:
            rows.append((int(size), name))
    if not rows:
        raise SystemExit(f"du read nothing under {path}: {r.stderr.strip()[:300]}")
    unreadable = sum(1 for l in r.stderr.splitlines() if "Permission denied" in l)

    total = next((s for s, n in rows if n.rstrip("/") == path.rstrip("/") or n == path), None)
    rows = [(s, n) for s, n in rows if n.rstrip("/") != path.rstrip("/")]
    rows.sort(reverse=True)
    print(f"{path}: {human(total) if total is not None else '?'} in total "
          f"(this filesystem only, depth {depth})")
    for size, name in rows[:top]:
        share = f"{100 * size / total:5.1f}%" if total else ""
        print(f"{human(size):>11}  {share}  {name}")
    if unreadable:
        print(f"({unreadable} director{'y' if unreadable == 1 else 'ies'} could not be read "
              "and are not counted — sizes there are lower than the truth)")


if __name__ == "__main__":
    main()
