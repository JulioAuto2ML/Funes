#!/usr/bin/env bash
# =============================================================================
# run_images.sh — make the day's LinkedIn illustrations, from cron.
# =============================================================================
# One GPU holds both Funes' model and the image model, and not at once. So the
# images are made in a window: take the GPU from the LLM, start ComfyUI, render,
# stop ComfyUI, give the GPU back. Funes cannot do this itself — it would be
# stopping the model it is running on — so this is a cron job beside it.
#
# The one property that matters: **the model always comes back.** The restore is
# in an EXIT trap that is armed before anything is stopped, so a failed render,
# a ComfyUI that never starts, or a kill signal all end with the LLM restarted.
#
# It does nothing — and never touches the GPU — when there is nothing to do:
# the day's issue was not sent, it has no image prompts, or every image exists.
# That makes it safe to run twice and safe to run on a day the newsletter failed.
#
# Usage:   run_images.sh [publication-id]          # default: ai-pulse
#
# Exit codes:
#   0  done (or nothing to do) — individual images may still have failed; the
#      log says which, and post_tweet.py posts those items text-only
#   1  generation, or ComfyUI, failed
#   2  the issue was never sent (or no run record) — nothing done
#   4  the GPU memory never freed after stopping the model
#   5  the model did not come back healthy — look at this first
#   6  another run is in progress
#
# Environment (defaults suit yoda; the *_CMD ones exist so tests can swap them):
#   FUNES_PUBLISH_DIR  default ~/Documents/X_posts
#   LLAMA_CTL          default ~/launch_llama.sh        LLM_MODEL  default qwen9b
#   COMFY_DIR / COMFY_PYTHON / COMFY_PORT / COMFY_ARGS
#   WAIT_SENT_S        how long to wait for the run record (default 3600)
#   GENERATE_TIMEOUT_S hard ceiling on rendering (default 2400)
#
# Crontab (yoda) — after the newsletter run, before the first LinkedIn slot:
#   0 11 * * * /home/julio/funes-julio/publishing/run_images.sh ai-pulse \
#                >> ~/Documents/X_posts/images_log.txt 2>&1
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PUBLICATION="${1:-ai-pulse}"
PUBLISH_DIR="${FUNES_PUBLISH_DIR:-$HOME/Documents/X_posts}"
DAY="${RUN_DATE:-$(date +%F)}"
RECORD="$PUBLISH_DIR/runs/$PUBLICATION/$DAY.json"

LLAMA_CTL="${LLAMA_CTL:-$HOME/launch_llama.sh}"
LLM_MODEL="${LLM_MODEL:-qwen9b}"
LLM_HEALTH_URL="${LLM_HEALTH_URL:-http://127.0.0.1:8080/health}"
HEALTH_CMD="${HEALTH_CMD:-curl -fsS -m 3 -o /dev/null $LLM_HEALTH_URL}"

COMFY_DIR="${COMFY_DIR:-$HOME/ComfyUI-Installs/ComfyUI/ComfyUI}"
COMFY_PYTHON="${COMFY_PYTHON:-$HOME/torch72-test/bin/python}"
COMFY_PORT="${COMFY_PORT:-8189}"
COMFY_ARGS="${COMFY_ARGS:---vram-headroom 2 --cpu-vae}"
COMFY_CMD="${COMFY_CMD:-cd $COMFY_DIR && exec $COMFY_PYTHON main.py --listen 127.0.0.1 --port $COMFY_PORT $COMFY_ARGS}"
COMFY_LOG="${COMFY_LOG:-$PUBLISH_DIR/comfy_images.log}"

GENERATE_CMD="${GENERATE_CMD:-python3 $SCRIPT_DIR/generate_images.py $DAY --dir $PUBLISH_DIR --publication $PUBLICATION --server http://127.0.0.1:$COMFY_PORT}"
GENERATE_TIMEOUT_S="${GENERATE_TIMEOUT_S:-2400}"

VRAM_USED_MB_CMD="${VRAM_USED_MB_CMD:-}"
VRAM_FREE_BELOW_MB="${VRAM_FREE_BELOW_MB:-2000}"
VRAM_WAIT_S="${VRAM_WAIT_S:-60}"
COMFY_WAIT_S="${COMFY_WAIT_S:-180}"
LLM_WAIT_S="${LLM_WAIT_S:-600}"
WAIT_SENT_S="${WAIT_SENT_S:-3600}"
POLL_S="${POLL_S:-30}"
LOCK_FILE="${LOCK_FILE:-/tmp/funes_run_images.lock}"

log() { echo "[$(date -Is)] $*"; }

vram_used_mb() {
    if [ -n "$VRAM_USED_MB_CMD" ]; then $VRAM_USED_MB_CMD; return; fi
    local f
    for f in /sys/class/drm/card*/device/mem_info_vram_used; do
        [ -r "$f" ] && { echo $(( $(cat "$f") / 1048576 )); return; }
    done
    echo 999999     # unknown counts as "not free": better to give up than to guess
}

record_status() {
    python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("status",""))' \
        "$RECORD" 2>/dev/null
}

echo "=== $(date -Is) run_images $PUBLICATION $DAY ==="

# fd 9 holds the lock. Nothing this script starts may inherit it: llama-server,
# started by `launch_llama.sh start`, outlives this script, and a daemon holding
# the descriptor holds the lock with it — every later run would exit 6. So each
# spawn below says `9>&-`.
exec 9>"$LOCK_FILE"
flock -n 9 || { log "another run_images is in progress"; exit 6; }

# ── Is there anything to do? Decided before the GPU is touched ───────────────
waited=0
until [ "$(record_status)" = "sent" ]; do
    if [ "$waited" -ge "$WAIT_SENT_S" ]; then
        log "no 'sent' run record at $RECORD after ${WAIT_SENT_S}s — no images today"
        exit 2
    fi
    sleep "$POLL_S"; waited=$(( waited + POLL_S ))
done

PENDING="$(python3 "$SCRIPT_DIR/generate_images.py" "$DAY" --dir "$PUBLISH_DIR" \
            --publication "$PUBLICATION" --pending)" || {
    log "could not read the issue; leaving the GPU alone"; exit 1; }
if [ "$PENDING" = "0" ]; then
    log "nothing to render (no image prompts, or all images exist)"
    exit 0
fi
log "$PENDING image(s) to render"

# ── The window. The trap is armed BEFORE anything is stopped ─────────────────
COMFY_PID=""
GEN_PID=""

stop_comfy() {
    [ -n "$COMFY_PID" ] || return 0
    if kill -0 "$COMFY_PID" 2>/dev/null; then
        kill -TERM "$COMFY_PID" 2>/dev/null
        for _ in $(seq 1 20); do kill -0 "$COMFY_PID" 2>/dev/null || break; sleep 1; done
        kill -KILL "$COMFY_PID" 2>/dev/null
    fi
    COMFY_PID=""
}

restore() {
    local rc=$?
    trap - EXIT
    [ -n "$GEN_PID" ] && kill -TERM "$GEN_PID" 2>/dev/null
    stop_comfy
    log "giving the GPU back: starting $LLM_MODEL"
    "$LLAMA_CTL" start "$LLM_MODEL" 9>&-
    local waited=0
    until $HEALTH_CMD 2>/dev/null; do
        if [ "$waited" -ge "$LLM_WAIT_S" ]; then
            log "FAILED: $LLM_MODEL not healthy after ${LLM_WAIT_S}s — Funes has no model"
            exit 5
        fi
        sleep "$POLL_S"; waited=$(( waited + POLL_S ))
    done
    log "$LLM_MODEL is healthy again (exit $rc)"
    exit "$rc"
}
trap restore EXIT
trap 'exit 130' INT TERM

log "stopping $LLM_MODEL"
"$LLAMA_CTL" stop "$LLM_MODEL" 9>&-

# `stop` only sends the kill; the memory is free when the driver says so.
waited=0
while [ "$(vram_used_mb)" -ge "$VRAM_FREE_BELOW_MB" ]; do
    if [ "$waited" -ge "$VRAM_WAIT_S" ]; then
        log "VRAM still in use after ${VRAM_WAIT_S}s ($(vram_used_mb) MB) — giving up"
        exit 4
    fi
    sleep 1; waited=$(( waited + 1 ))
done
log "VRAM free ($(vram_used_mb) MB used)"

log "starting ComfyUI on :$COMFY_PORT"
setsid bash -c "$COMFY_CMD" >> "$COMFY_LOG" 2>&1 9>&- &
COMFY_PID=$!

waited=0
until curl -fsS -m 2 -o /dev/null "http://127.0.0.1:$COMFY_PORT/system_stats" 2>/dev/null; do
    if ! kill -0 "$COMFY_PID" 2>/dev/null; then
        log "ComfyUI exited during startup (see $COMFY_LOG)"; exit 1
    fi
    if [ "$waited" -ge "$COMFY_WAIT_S" ]; then
        log "ComfyUI not answering after ${COMFY_WAIT_S}s"; exit 1
    fi
    sleep 1; waited=$(( waited + 1 ))
done
log "ComfyUI is up; rendering"

# Backgrounded and waited on, not run in the foreground: bash holds a trap until
# the foreground command ends, so a SIGTERM would otherwise have waited out the
# whole batch with the model still stopped.
timeout "$GENERATE_TIMEOUT_S" $GENERATE_CMD 9>&- &
GEN_PID=$!
wait "$GEN_PID"
rc=$?
GEN_PID=""
if [ "$rc" -eq 124 ]; then
    log "rendering hit the ${GENERATE_TIMEOUT_S}s ceiling; posts without an image go out text-only"
fi
[ "$rc" -eq 0 ] || exit 1
log "rendering finished"
exit 0
