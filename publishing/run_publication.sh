#!/usr/bin/env bash
# =============================================================================
# run_publication.sh — produce and send one issue, from cron.
# =============================================================================
# Funes has no scheduler of its own. That is a real gap — a scheduler wants a
# job table, retries and a UI — but the gap that mattered was smaller and this
# closes it: the newsletter was started by hand while the LinkedIn posts that
# consume it were on cron, so a failed newsletter still left cron posting
# yesterday's items.
#
# So: this runs the curator, and then decides whether it worked by reading the
# run record rather than by believing the agent. The SSE stream is logged and
# otherwise ignored; the exit code comes from the file the publisher wrote.
# publishing/post_tweet.py reads the same record before every LinkedIn post, so
# both ends of the pipeline agree on what "today went out" means.
#
# Usage:
#   run_publication.sh [publication-id]        # default: ai-pulse
#
# Environment:
#   FUNES_URL          default http://127.0.0.1:8484
#   FUNES_PUBLISH_DIR  where runs/ lives; default ~/Documents/X_posts
#
# Exit codes:
#   0  the issue was sent
#   1  could not reach Funes
#   2  the run finished without sending (the record says why)
#   3  no run record at all — the agent never got as far as publishing
#
# Crontab (yoda), with the LinkedIn posts that depend on it downstream:
#   0 9 * * * /path/to/Funes/publishing/run_publication.sh ai-pulse \
#               >> ~/Documents/X_posts/publication_log.txt 2>&1

set -uo pipefail

PUBLICATION="${1:-ai-pulse}"
FUNES_URL="${FUNES_URL:-http://127.0.0.1:8484}"
PUBLISH_DIR="${FUNES_PUBLISH_DIR:-$HOME/Documents/X_posts}"
TODAY="$(date +%F)"
RECORD="$PUBLISH_DIR/runs/$PUBLICATION/$TODAY.json"

echo "=== $(date -Is) $PUBLICATION ==="

# One turn is one issue. The session id is the date so a manual re-run on the
# same day continues rather than starting over.
if ! curl -sS --fail-with-body --max-time 1800 \
        -H 'Content-Type: application/json' \
        -X POST "$FUNES_URL/api/chat" \
        -d "$(printf '{"agent":"curator","session":"%s-%s","message":"Produce and send today'"'"'s issue of %s."}' \
              "$PUBLICATION" "$TODAY" "$PUBLICATION")"; then
    echo "FAILED: could not reach Funes at $FUNES_URL"
    exit 1
fi
echo

# What the agent said about itself is not evidence. What the publisher wrote is.
if [ ! -f "$RECORD" ]; then
    echo "FAILED: no run record at $RECORD — nothing was published for $TODAY."
    exit 3
fi

STATUS="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("status",""))' "$RECORD")"
if [ "$STATUS" != "sent" ]; then
    echo "NOT SENT: $RECORD says status=$STATUS"
    cat "$RECORD"
    exit 2
fi

echo "SENT: $RECORD"
cat "$RECORD"
