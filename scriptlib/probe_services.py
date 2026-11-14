#!/usr/bin/env python3
"""Self-healing probe (H0): the services Funes depends on, and disk space.

One check each: the model (llama-server, FUNES_LLM_URL), the embedding
server (FUNES_EMBED_URL), both WhatsApp bridges, and free space where data/
lives. Prints {"checks": [...]} for the server's probe runner (src/core/probes.h);
a check whose URL is not configured is left out rather than reported broken.

Read-only: one GET per service, a statvfs. No model.
"""

import json
import os
import shutil
import sys
import urllib.error
import urllib.parse
import urllib.request

TIMEOUT = 8


def base_url(url: str) -> str:
    """http://localhost:8080/v1 → http://localhost:8080 (llama-server's /health
    is at the root, whatever path the OpenAI-compatible API is under)."""
    p = urllib.parse.urlsplit(url)
    return f"{p.scheme}://{p.netloc}" if p.scheme and p.netloc else ""


def http_answers(url: str, path: str):
    """(answers, detail). Any HTTP status counts as answering for a bridge;
    the model servers are asked for /health and must say 200."""
    try:
        with urllib.request.urlopen(url + path, timeout=TIMEOUT) as r:
            return True, f"HTTP {r.status}"
    except urllib.error.HTTPError as e:
        return False, f"HTTP {e.code}"
    except (urllib.error.URLError, OSError, ValueError) as e:
        reason = getattr(e, "reason", e)
        return False, f"{type(reason).__name__}: {reason}"


def model_check(name: str, label: str, url: str, severity: str):
    base = base_url(url)
    if not base:
        return None
    ok, detail = http_answers(base, "/health")
    return {"name": name, "ok": ok, "severity": severity,
            "summary": f"{label} at {base} " + ("answers" if ok else "does not answer"),
            "evidence": "" if ok else f"GET {base}/health → {detail}"}


def bridge_check(name: str, label: str, url: str):
    base = base_url(url)
    if not base:
        return None
    ok, detail = http_answers(base, "/")
    # A bridge serves only /api/*: a 404 on / is a bridge that is up.
    if not ok and detail.startswith("HTTP "):
        ok = True
    return {"name": name, "ok": ok, "severity": "warning",
            "summary": f"{label} at {base} " + ("answers" if ok else "does not answer"),
            "evidence": "" if ok else f"GET {base}/ → {detail}"}


def disk_check(path: str):
    if not path or not os.path.isdir(path):
        return None
    u = shutil.disk_usage(path)
    free = u.free / u.total * 100 if u.total else 100.0
    gb = u.free / 2**30
    severity = "critical" if free < 3 else "warning"
    return {"name": "disk", "ok": free >= 10, "severity": severity,
            "summary": f"{free:.0f}% ({gb:.1f} GB) free on the disk holding {path}",
            "evidence": "" if free >= 10 else f"{u.used / 2**30:.1f} of {u.total / 2**30:.1f} GB used"}


def collect(env) -> list:
    checks = [
        model_check("llm", "The model server (llama-server)", env.get("LLM_URL", ""), "critical"),
        model_check("embeddings", "The embedding server", env.get("EMBED_URL", ""), "warning"),
        bridge_check("bridge-funes", "Funes's WhatsApp bridge", env.get("FUNES_BRIDGE_URL", "")),
        bridge_check("bridge-personal", "The personal WhatsApp bridge",
                     env.get("PERSONAL_BRIDGE_URL", "")),
        disk_check(env.get("DATA_DIR", "")),
    ]
    return [c for c in checks if c]


def main() -> int:
    json.dump({"checks": collect(os.environ)}, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
