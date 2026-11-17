"""Which cells must not be read as results.

A web cell is INVALID when a search in it failed: its own `search.errors` (counted since the
Parallel-error stop, `ko/tools.py`), or an entry in `results/search_audit.json` for cells written
before the counter existed (made by `web_payload_audit.py` from the raw payloads, which are the
evidence: failed_searches 0 = checked clean, > 0 = INVALID). An absent `search.errors` on a web cell with no annotation is *unchecked*, not valid.
"""
import json
from pathlib import Path


def annotations(results: Path) -> dict:
    p = Path(results) / "search_audit.json"
    return json.loads(p.read_text()) if p.exists() else {}


def status(r: dict, file_name: str, notes: dict) -> tuple[str, str]:
    """('ok'|'INVALID'|'unchecked', reason). Only web conditions can be invalid this way."""
    if r.get("condition", "").split("/")[0] not in ("web", "web+cache", "web+writeback"):
        return "ok", ""
    if file_name in notes:
        n = notes[file_name]
        if n["failed_searches"]:
            return "INVALID", f"{n['failed_searches']} of {n['searches']} searches failed ({n['reason']})"
        return "ok", ""
    s = r.get("search") or {}
    if s.get("errors", 0) > 0:
        return "INVALID", f"{s['errors']} search error(s): {(s.get('error_log') or [{}])[0].get('text', '')[:120]}"
    if "errors" not in s:
        return "unchecked", "cell predates search-error counting; no payload audit annotation"
    return "ok", ""
