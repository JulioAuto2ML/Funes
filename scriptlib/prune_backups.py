#!/usr/bin/env python3
"""Delete old files from data/backups, keeping the newest ones.

--older_than_days=N   only entries last modified more than N days ago (at least 7; default 30)
--keep=K              never the K newest entries, however old (at least 1; default 3)
--dry_run             say what would go, delete nothing

BACKUPS_DIR comes from the manifest. Only top-level entries are considered,
and a directory goes as a whole. Classed `host` (effects.h): after outside
content, a call runs against an overlay and the person approves the deletions
it made (staging.h).
"""
import os
import shutil
import sys
import time


def parse_args(argv):
    args = {}
    for token in argv:
        if not token.startswith("--"):
            raise SystemExit(f"unexpected argument: {token}")
        key, sep, value = token[2:].partition("=")
        args[key] = value if sep else True
    return args


def number(args, key, default, minimum):
    try:
        v = int(float(args.get(key, default)))
    except ValueError:
        raise SystemExit(f"{key} must be a number")
    if v < minimum:
        raise SystemExit(f"{key} must be at least {minimum}")
    return v


def size_of(path):
    if os.path.islink(path) or not os.path.isdir(path):
        return os.lstat(path).st_size
    total = 0
    for dirpath, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(dirpath, f)).st_size
            except OSError:
                pass
    return total


def main():
    args = parse_args(sys.argv[1:])
    days = number(args, "older_than_days", 30, 7)
    keep = number(args, "keep", 3, 1)
    dry = args.get("dry_run") is True or args.get("dry_run") == "true"
    root = os.environ.get("BACKUPS_DIR", "")
    if not root or not os.path.isdir(root):
        raise SystemExit(f"no backups directory: {root or '(BACKUPS_DIR unset)'}")

    entries = []
    for name in os.listdir(root):
        p = os.path.join(root, name)
        entries.append((os.lstat(p).st_mtime, name, p))
    entries.sort(reverse=True)                       # newest first
    cutoff = time.time() - days * 86400
    doomed = [e for e in entries[keep:] if e[0] < cutoff]

    freed = 0
    for mtime, name, p in doomed:
        size = size_of(p)
        freed += size
        stamp = time.strftime("%Y-%m-%d", time.localtime(mtime))
        print(f"{'would delete' if dry else 'deleted'}  {name}  ({size / 2**20:.1f} MiB, {stamp})")
        if not dry:
            if os.path.isdir(p) and not os.path.islink(p):
                shutil.rmtree(p)
            else:
                os.unlink(p)
    kept = len(entries) - len(doomed)
    print(f"{'would free' if dry else 'freed'} {freed / 2**20:.1f} MiB; {len(doomed)} removed, {kept} kept "
          f"(older than {days} days, newest {keep} always kept)")


if __name__ == "__main__":
    main()
