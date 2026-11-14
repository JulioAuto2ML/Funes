#!/usr/bin/env python3
"""Self-healing probe (H0): can this machine reach the internet?

One check: a TCP connection on 443 to any of a few hosts. On 2026-11-14 yoda
lost the internet because its default route went out the wired link through
anakin, whose uplink was down, and the first sign was a failed `git pull` in a
Claude session. The evidence names the interface the default route uses, which
is usually the answer.

Read-only: /proc/net/route and a few connect() calls. No model.
"""

import json
import os
import socket
import sys

HOSTS = ["github.com", "api.linkedin.com", "www.google.com"]
TIMEOUT = 6


def default_routes(route_file="/proc/net/route") -> list:
    """Interfaces with a default route, best (lowest metric) first."""
    try:
        lines = open(route_file, encoding="ascii").read().splitlines()[1:]
    except OSError:
        return []
    found = []
    for line in lines:
        f = line.split()
        # Iface Destination Gateway Flags RefCnt Use Metric Mask …
        if len(f) >= 8 and f[1] == "00000000" and f[7] == "00000000":
            found.append((int(f[6]), f[0]))
    return [iface for _, iface in sorted(found)]


def reachable(host: str) -> str:
    """"" when a connection on 443 succeeds, else why not."""
    try:
        with socket.create_connection((host, 443), timeout=TIMEOUT):
            return ""
    except OSError as e:
        return f"{host}: {type(e).__name__}: {e}"


def collect(hosts=HOSTS, route_file="/proc/net/route", connect=reachable) -> list:
    errors = []
    for h in hosts:
        err = connect(h)
        if not err:
            return [{"name": "internet", "ok": True, "summary": f"The internet is reachable ({h}:443)"}]
        errors.append(err)
    routes = default_routes(route_file)
    via = ", ".join(routes) if routes else "none"
    return [{"name": "internet", "ok": False, "severity": "warning",
             "summary": f"No internet: none of {', '.join(hosts)} answers on 443 "
                        f"(default route via {via})",
             "evidence": "\n".join(errors)}]


def main() -> int:
    hosts = [h for h in os.environ.get("PROBE_HOSTS", "").split(",") if h] or HOSTS
    json.dump({"checks": collect(hosts)}, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
