#!/usr/bin/env bash
# =============================================================================
# scripts/deploy_alternating_editions.sh — one-off, yoda, 2026-11-02 02:00
# =============================================================================
# From Monday 2026-11-02 the newsletter alternates: English (ai-pulse) Mon/Wed/
# Fri, Spanish (ai-pulse-es) Tue/Thu/Sat, and on Sunday no issue but one LinkedIn
# carousel per edition (publishing/weekly_carousel.py). This puts that live:
#
#   1. git pull --ff-only, rebuild (the binary gains `funes cron-schedule`)
#   2. the two Funes cron jobs that produce the issues get their new days —
#      looked up by name and task, never trusted by id alone
#   3. the crontab (images, LinkedIn posts, carousels) is replaced by
#      scripts/yoda.crontab, the old one saved to data/backups/
#   4. funes restarts on the new binary
#
# Run by a transient systemd timer at 02:00, when nothing is scheduled; the log
# is data/backups/deploy-alternating-editions.log. Safe to re-run: every step
# sets a state rather than applying a change.
#
#   systemd-run --user --on-calendar='2026-11-02 02:00:00' \
#       --unit=funes-deploy-alternating /bin/bash -lc \
#       'cd ~/Funes && git pull --ff-only && exec scripts/deploy_alternating_editions.sh'
# =============================================================================

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
export FUNES_DB="${FUNES_DB:-$ROOT/data/memory.db}"
LOG="$ROOT/data/backups/deploy-alternating-editions.log"
mkdir -p "$ROOT/data/backups"
exec >>"$LOG" 2>&1

log() { echo "[$(date -Is)] $*"; }
log "=== deploy alternating editions ($(git rev-parse --short HEAD)) ==="

git pull --ff-only
cmake --build build -j"$(nproc)"
log "built $(./bin/funes --version)"

# Job ids as of 2026-10-29 are 2 (English) and 3 (Spanish). Find them by what
# they do, so a job recreated since then is still the one changed — and a
# missing one stops the deploy before anything else is touched.
job_id() {
    python3 - "$FUNES_DB" "$1" <<'EOF'
import sqlite3, sys
db, task = sys.argv[1], sys.argv[2]
rows = sqlite3.connect(f"file:{db}?mode=ro", uri=True).execute(
    "SELECT id FROM cron_jobs WHERE agent = 'curator' AND task = ?", (task,)).fetchall()
if len(rows) != 1:
    sys.exit(f"expected one curator job with task {task!r}, found {len(rows)}")
print(rows[0][0])
EOF
}
EN_ID="$(job_id "Publish today's ai-pulse issue")"
ES_ID="$(job_id "Publish today's ai-pulse-es issue")"

./bin/funes cron-schedule "$EN_ID" "0 9 * * 1,3,5"
./bin/funes cron-schedule "$ES_ID" "30 10 * * 2,4,6"

BACKUP="$ROOT/data/backups/crontab-$(date +%Y%m%d-%H%M%S)"
crontab -l > "$BACKUP" || true
crontab "$ROOT/scripts/yoda.crontab"
log "crontab installed (previous one in $BACKUP)"

systemctl --user restart funes
for _ in $(seq 1 30); do
    if curl -fsS -m 2 -o /dev/null http://127.0.0.1:8485/api/auth/status; then
        log "funes is up — done"
        exit 0
    fi
    sleep 2
done
log "FAILED: funes did not answer on :8485 after the restart"
exit 1
