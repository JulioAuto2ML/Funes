#!/usr/bin/env python3
"""JevBench runner for Funes's `classifier` agent (classify_decision tool).

Two subcommands, deliberately split so a slow, real-LLM run is never repeated
just to change how the numbers are computed:

    run        hits a live Funes instance once per decision, writes raw
               per-item results (predicted distribution, latency, errors)
    summarize  joins those results back against the dataset and computes
               accuracy, calibration and the rest from the raw file

Talks to a running Funes instance over its normal HTTP API (POST /api/login,
POST /api/chat) - stdlib only (urllib, http.cookiejar), matching this
repo's other Python dev tools (tests/mock_llm.py, tests/expire_token.py).

Each decision is sent with its own session id so items never share
conversation history - classify_decision is meant to be a stateless
judgement per call, and a shared session would let leftover context from
item N leak into item N+1's prompt.

See README.md in this directory for the dataset's provenance, the exact
metrics computed and why "JevBench Score" (Intelligence/Calibration/
Speed/Cost) is not reproduced here.
"""

import argparse
import getpass
import http.client
import http.cookiejar
import json
import statistics
import sys
import time
import urllib.error
import urllib.request


# ── dataset ──────────────────────────────────────────────────────────────────

def load_dataset(paths):
    records = []
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                record = json.loads(line)
                record["_source_file"] = path
                records.append(record)
    return records


def label_criteria(record):
    """Returns {label: criterion text}, handling the three shapes JevBench
    uses: noul's {"true":.., "false":..} keyed by boolean name rather than
    the actual label strings, choice's {label: text}, and score's list
    positionally aligned with `labels`. Falls back to {} (no criteria shown)
    rather than raising, so one odd record degrades gracefully."""
    labels = record["labels"]
    criteria = record["question"].get("criteria")
    qtype = record["question"]["type"]

    if not criteria:
        return {}
    if qtype == "score" and isinstance(criteria, list):
        return dict(zip(labels, criteria))
    if qtype == "noul" and isinstance(criteria, dict):
        out = {}
        for label in labels:
            key = "true" if label.lower() == "yes" else "false" if label.lower() == "no" else None
            if key and key in criteria:
                out[label] = criteria[key]
        return out
    if isinstance(criteria, dict):
        return {label: criteria[label] for label in labels if label in criteria}
    return {}


def build_task(record):
    """State:/Question:/Options: - the exact shape agents/classifier.yaml's
    delegation_notes documents. The rubric (question.criteria) is folded
    into the Question line rather than sent as a separate field, since the
    agent's prompt only knows to parse those three lines - this keeps the
    benchmark from needing any change to classify_decision or the agent."""
    labels = record["labels"]
    instructions = record["question"]["instructions"]
    crit = label_criteria(record)

    question = instructions
    if crit:
        defs = "; ".join(f"{label} = {text.strip()}" for label, text in crit.items())
        question = f"{instructions} (Definitions: {defs})"

    return f"State: {record['state']}\nQuestion: {question}\nOptions: " + " | ".join(labels)


# ── Funes HTTP client ────────────────────────────────────────────────────────

class FunesClient:
    def __init__(self, base_url, timeout=300):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def _post(self, path, body, stream=False):
        req = urllib.request.Request(
            self.base_url + path,
            data=json.dumps(body).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        return self.opener.open(req, timeout=self.timeout)

    def login(self, username, password):
        resp = self._post("/api/login", {"username": username, "password": password})
        data = json.loads(resp.read())
        if not data.get("ok"):
            raise RuntimeError(f"login failed: {data}")

    def chat(self, agent, message, session):
        """POST /api/chat and parse the SSE stream. Returns the 'done'
        event's text (the classifier's final answer, expected to be the
        classify_decision JSON verbatim per the agent's system prompt)."""
        resp = self._post("/api/chat", {"agent": agent, "message": message, "session": session})
        done_text = None
        event = None
        for raw_line in resp:
            line = raw_line.decode("utf-8", errors="replace").rstrip("\n")
            if line.startswith("event:"):
                event = line[len("event:"):].strip()
            elif line.startswith("data:") and event == "done":
                payload = json.loads(line[len("data:"):].strip())
                done_text = payload.get("text", "")
        if done_text is None:
            raise RuntimeError("no 'done' event in the chat stream")
        return done_text


# ── run ──────────────────────────────────────────────────────────────────────

def cmd_run(args):
    records = load_dataset(args.dataset)
    if args.limit:
        records = records[: args.limit]

    client = FunesClient(args.base_url, timeout=args.timeout)
    password = args.password or getpass.getpass(f"Password for {args.username}: ")
    client.login(args.username, password)

    print(f"— running {len(records)} decisions against {args.base_url} "
          f"(agent={args.agent}, delay={args.delay_s}s)", file=sys.stderr)

    with open(args.output, "w", encoding="utf-8") as out:
        for i, record in enumerate(records):
            task = build_task(record)
            session = f"jevbench-{record['id']}"
            row = {"id": record["id"]}
            t0 = time.monotonic()
            try:
                text = client.chat(args.agent, task, session)
                parsed = json.loads(text)
                row["predicted_top"] = parsed.get("top")
                row["predicted_probs"] = {
                    o["option"]: o["probability"] for o in parsed.get("options", [])
                }
                row["error"] = None
            except (urllib.error.URLError, http.client.HTTPException,
                    json.JSONDecodeError, RuntimeError, KeyError) as e:
                row["predicted_top"] = None
                row["predicted_probs"] = {}
                row["error"] = f"{type(e).__name__}: {e}"
            row["latency_s"] = time.monotonic() - t0
            out.write(json.dumps(row) + "\n")
            out.flush()

            status = "ok" if row["error"] is None else f"ERROR: {row['error']}"
            print(f"  [{i + 1}/{len(records)}] {record['id']}: {status}", file=sys.stderr)
            if args.delay_s:
                time.sleep(args.delay_s)

    print(f"— wrote {args.output}", file=sys.stderr)


# ── summarize ────────────────────────────────────────────────────────────────

def brier_score(probs, labels, expected_label):
    """sum_k (p_k - y_k)^2 over the exact label set - JevBench's definition."""
    return sum(
        (probs.get(label, 0.0) - (1.0 if label == expected_label else 0.0)) ** 2
        for label in labels
    )


def expected_calibration_error(confidences_correct, n_bins=10):
    """Top-label confidence, n equal-width bins, empty bins excluded -
    JevBench's stated method. confidences_correct is a list of
    (confidence, was_correct) pairs."""
    bins = [[] for _ in range(n_bins)]
    for conf, correct in confidences_correct:
        idx = min(int(conf * n_bins), n_bins - 1)
        bins[idx].append((conf, correct))

    total = len(confidences_correct)
    if total == 0:
        return None
    ece = 0.0
    for bucket in bins:
        if not bucket:
            continue
        avg_conf = sum(c for c, _ in bucket) / len(bucket)
        avg_acc = sum(1.0 for _, ok in bucket if ok) / len(bucket)
        ece += (len(bucket) / total) * abs(avg_conf - avg_acc)
    return ece


def majority_class_accuracy(expected_labels):
    if not expected_labels:
        return None
    counts = {}
    for e in expected_labels:
        counts[e] = counts.get(e, 0) + 1
    return max(counts.values()) / len(expected_labels)


def summarize_group(rows):
    """rows: list of dicts with expected, labels, predicted_top,
    predicted_probs, error, qtype, latency_s. Returns a metrics dict."""
    n = len(rows)
    valid = [r for r in rows if r["error"] is None]
    invalid_rate = (n - len(valid)) / n if n else None

    n_correct = sum(1 for r in valid if str(r["predicted_top"]) == str(r["expected"]))
    accuracy = n_correct / len(valid) if valid else None

    briers = [
        brier_score(r["predicted_probs"], r["labels"], str(r["expected"]))
        for r in valid
    ]
    brier = statistics.mean(briers) if briers else None

    conf_correct = []
    for r in valid:
        if not r["predicted_probs"]:
            continue
        top_label, top_conf = max(r["predicted_probs"].items(), key=lambda kv: kv[1])
        conf_correct.append((top_conf, str(top_label) == str(r["expected"])))
    ece = expected_calibration_error(conf_correct)

    ordinal = [r for r in valid if r["qtype"] == "score"]
    mae = None
    if ordinal:
        errs = []
        for r in ordinal:
            # probability-weighted level vs. the reference level (JevBench's
            # ordinal MAE) - labels are the string forms of 0..k.
            weighted = sum(float(lbl) * p for lbl, p in r["predicted_probs"].items())
            errs.append(abs(weighted - float(r["expected"])))
        mae = statistics.mean(errs)

    latencies = [r["latency_s"] for r in rows if r.get("latency_s") is not None]

    return {
        "n": n,
        "invalid_rate": invalid_rate,
        "accuracy": accuracy,
        "majority_class_accuracy": majority_class_accuracy([str(r["expected"]) for r in valid]),
        "brier": brier,
        "ece": ece,
        "ordinal_mae": mae,
        "latency_mean_s": statistics.mean(latencies) if latencies else None,
        "latency_p90_s": (sorted(latencies)[int(len(latencies) * 0.9)]
                          if len(latencies) >= 10 else None),
    }


def fmt(v, pct=False):
    if v is None:
        return "—"
    return f"{v * 100:.1f}%" if pct else f"{v:.3f}"


def print_table(title, groups):
    print(f"\n## {title}\n")
    header = f"{'group':<20} {'n':>4} {'acc':>7} {'maj.acc':>8} {'brier':>7} {'ece':>7} {'ord.mae':>8} {'invalid':>8} {'lat(s)':>7}"
    print(header)
    print("-" * len(header))
    for name, m in groups:
        print(f"{name:<20} {m['n']:>4} {fmt(m['accuracy'], True):>7} "
              f"{fmt(m['majority_class_accuracy'], True):>8} {fmt(m['brier']):>7} "
              f"{fmt(m['ece']):>7} {fmt(m['ordinal_mae']):>8} "
              f"{fmt(m['invalid_rate'], True):>8} {fmt(m['latency_mean_s']):>7}")


def cmd_summarize(args):
    records = {r["id"]: r for r in load_dataset(args.dataset)}
    rows = []
    with open(args.results, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            result = json.loads(line)
            record = records.get(result["id"])
            if record is None:
                print(f"warning: {result['id']} not found in dataset, skipping", file=sys.stderr)
                continue
            rows.append({
                "id": result["id"],
                "expected": record["expected"],
                "labels": record["labels"],
                "family": record["family"],
                "qtype": record["question"]["type"],
                "source_file": record["_source_file"],
                "predicted_top": result.get("predicted_top"),
                "predicted_probs": result.get("predicted_probs") or {},
                "error": result.get("error"),
                "latency_s": result.get("latency_s"),
            })

    by_family = {}
    by_file = {}
    for r in rows:
        by_family.setdefault(r["family"], []).append(r)
        by_file.setdefault(r["source_file"], []).append(r)

    print_table("Overall", [("all", summarize_group(rows))])
    print_table("By file", sorted(
        ((name, summarize_group(group)) for name, group in by_file.items()),
        key=lambda kv: -kv[1]["n"],
    ))
    print_table("By family", sorted(
        ((name, summarize_group(group)) for name, group in by_family.items()),
        key=lambda kv: -kv[1]["n"],
    ))

    if args.report:
        report = {
            "overall": summarize_group(rows),
            "by_file": {name: summarize_group(group) for name, group in by_file.items()},
            "by_family": {name: summarize_group(group) for name, group in by_family.items()},
        }
        with open(args.report, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
        print(f"\n— wrote {args.report}", file=sys.stderr)


# ── CLI ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_run = sub.add_parser("run", help="hit a live Funes instance, write raw per-item results")
    p_run.add_argument("--dataset", nargs="+", required=True, help="one or more .jsonl files")
    p_run.add_argument("--base-url", default="http://127.0.0.1:8484")
    p_run.add_argument("--agent", default="classifier")
    p_run.add_argument("--username", required=True)
    p_run.add_argument("--password", default=None, help="omit to be prompted (recommended)")
    p_run.add_argument("--output", required=True, help="path to write results.jsonl")
    p_run.add_argument("--limit", type=int, default=None, help="only run the first N items (smoke test)")
    p_run.add_argument("--delay-s", type=float, default=0.2, help="pause between calls (shared GPU)")
    p_run.add_argument("--timeout", type=float, default=300,
                       help="per-request socket timeout — long_policy states can push a single "
                            "call well past 120s once the model actually finishes echoing a "
                            "~15K-character state back as a tool-call argument (default 300)")
    p_run.set_defaults(func=cmd_run)

    p_sum = sub.add_parser("summarize", help="compute metrics from a results.jsonl")
    p_sum.add_argument("--dataset", nargs="+", required=True)
    p_sum.add_argument("--results", required=True)
    p_sum.add_argument("--report", default=None, help="optional path to write a JSON report")
    p_sum.set_defaults(func=cmd_summarize)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
