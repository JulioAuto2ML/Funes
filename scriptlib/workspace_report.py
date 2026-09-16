#!/usr/bin/env python3
"""Summarize the workspace this script was started in.

Arguments arrive as `--name=value` tokens (a bare `--name` for a true
boolean) — see scriptlib/README.md. The working directory is already the
calling account's workspace, so this script never takes, or trusts, an
absolute path: `--path` is resolved inside the cwd and refused if it climbs
out, which keeps one account's report from reading another's files even
though the script itself runs unsandboxed.
"""
import os
import sys


def parse_args(argv):
    args = {}
    for token in argv:
        if not token.startswith("--"):
            raise SystemExit(f"unexpected argument: {token}")
        key, sep, value = token[2:].partition("=")
        args[key] = value if sep else True
    return args


def main():
    args = parse_args(sys.argv[1:])
    root = os.path.realpath(os.getcwd())
    target = os.path.realpath(os.path.join(root, args.get("path", "") or ""))
    if target != root and not target.startswith(root + os.sep):
        raise SystemExit("path must stay inside the workspace")
    if not os.path.isdir(target):
        raise SystemExit(f"no such directory in the workspace: {args.get('path')}")

    files = []
    for dirpath, _dirnames, filenames in os.walk(target):
        for name in filenames:
            full = os.path.join(dirpath, name)
            if os.path.islink(full) or not os.path.isfile(full):
                continue
            files.append((os.path.getsize(full), os.path.relpath(full, root)))

    total = sum(size for size, _ in files)
    where = os.path.relpath(target, root)
    print(f"directory: {'.' if where == '.' else where}")
    print(f"files: {len(files)}")
    print(f"total size: {total / 1024:.1f} KB")

    if files and args.get("details"):
        print("largest files:")
        for size, rel in sorted(files, reverse=True)[:20]:
            print(f"  {size / 1024:8.1f} KB  {rel}")


if __name__ == "__main__":
    main()
