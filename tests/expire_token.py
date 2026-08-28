#!/usr/bin/env python3
"""Backdate one auth token's expiry, for tests/integration.sh.

Session tokens live 30 days, so the only way to test expiry over HTTP in a
few seconds is to move the row's clock rather than wait. Deliberately a
separate file: the assertion it supports is that an *expired* token stops
authenticating, which is a different failure from a *deleted* one, and doing
this inline in the shell script would have hidden the difference.
"""
import sqlite3
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: expire_token.py <db> <token>", file=sys.stderr)
        return 2
    db, token = sys.argv[1], sys.argv[2]

    # The server holds this database open in WAL mode; the timeout covers the
    # moment a request is mid-write.
    con = sqlite3.connect(db, timeout=10)
    try:
        con.execute(
            "UPDATE auth_tokens SET expires_at = datetime('now', '-1 day') "
            "WHERE token = ?", (token,))
        con.commit()
        # The row must still be there. If the UPDATE matched nothing, or the
        # row were gone, the caller's assertion would pass for the wrong
        # reason — "no such token" and "expired token" look identical from
        # outside, which is the whole point of resolve_token's design.
        rows = con.execute(
            "SELECT COUNT(*) FROM auth_tokens WHERE token = ? "
            "AND expires_at <= datetime('now')", (token,)).fetchone()[0]
        if rows != 1:
            print(f"expire_token: token row not found or not expired ({rows})",
                  file=sys.stderr)
            return 1
    finally:
        con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
