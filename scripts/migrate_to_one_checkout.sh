#!/usr/bin/env bash
# =============================================================================
# scripts/migrate_to_one_checkout.sh — one-off, yoda, 5.2
# =============================================================================
# Moves everything the 5.1 layout spread around $HOME into ~/Funes:
#
#   ~/.funes-v4/memory.db{,-wal,-shm}   → data/memory.db…   (older copies → data/backups/)
#   ~/.funes-v4/workspace               → data/workspace
#   ~/.funes-v4/samsung_tv_token*, tv_weather_art → data/tv/
#   ~/.funes-v4/public                  → data/public       (tailscale funnel /noticias)
#   ~/.funes-v4/whatsapp_autoresponder_state.json → data/whatsapp/
#   ~/.funes-v4/bench                   → data/bench
#   ~/Documents/X_posts                 → data/newsletter   (incl. .env)
#   ~/whatsapp-bridge/{store,store-funes} → data/whatsapp/
#   ~/whatsapp-bridge/whatsapp-bridge   → third-party/whatsapp-mcp/whatsapp-bridge/
#   ~/funes-julio's untracked runtime bits (scriptlib/.venv, node_modules,
#     the MCP server's .venv)           → same paths in ~/Funes
#   config/funes.local.bak-*            → config/backups/
#
# then rewrites funes.local and the crontab, swaps funes-v4 /
# whatsapp-autoresponder-v4 for funes / whatsapp-autoresponder, restarts the
# bridges from their new home and repoints the tailscale funnel.
#
# Precondition: ~/Funes is pulled and built (bin/funes is the 5.2 binary).
# Nothing is deleted: ~/funes-julio and ~/whatsapp-bridge are renamed
# *.retired-<date>, and a tarball of the state is written first.
# =============================================================================
set -euo pipefail

FUNES=/home/julio/Funes
DATA=$FUNES/data
OLD=/home/julio/.funes-v4
STAMP=$(date +%Y%m%d-%H%M%S)
BACKUP=/home/julio/funes-premerge-backup-$STAMP.tar.gz

log() { echo "[migrate] $*"; }
mv_if() { if [ -e "$1" ] || [ -L "$1" ]; then mkdir -p "$(dirname "$2")"; mv -n "$1" "$2"; log "moved $1 → $2"; fi; }

[ -d "$OLD" ] || { log "$OLD is gone — already migrated?"; exit 1; }
[ -e "$DATA/memory.db" ] && { log "$DATA/memory.db already exists — refusing to overwrite"; exit 1; }
[ -x "$FUNES/bin/funes" ] || { log "build $FUNES first"; exit 1; }
grep -q 'FUNES_ROOT' "$FUNES/agents/curator.yaml" || { log "$FUNES is not the 5.2 tree — git pull first"; exit 1; }

# Don't move a directory out from under a publication or image run.
if pgrep -f 'publish_issue.py|publish_newsletter.py|run_images.sh|post_tweet.py' >/dev/null; then
    log "a newsletter job is running — try again in a few minutes"; exit 1
fi

log "stopping services"
systemctl --user stop whatsapp-autoresponder-v4 funes-v4 whatsapp-bridge whatsapp-bridge-funes

log "backup → $BACKUP"
tar -czf "$BACKUP" -C /home/julio \
    .funes-v4 whatsapp-bridge/store whatsapp-bridge/store-funes \
    Funes/config/funes.local Documents/X_posts/.env
crontab -l > "/home/julio/crontab.bak-$STAMP"

mkdir -p "$DATA"/{backups,tv,whatsapp,public} "$FUNES/config/backups"

# ── ~/.funes-v4 ──────────────────────────────────────────────────────────────
for f in memory.db memory.db-wal memory.db-shm; do mv_if "$OLD/$f" "$DATA/$f"; done
for f in "$OLD"/memory.db.*; do mv_if "$f" "$DATA/backups/$(basename "$f")"; done
mv_if "$OLD/workspace"                          "$DATA/workspace"
mv_if "$OLD/samsung_tv_token"                   "$DATA/tv/samsung_tv_token"
mv_if "$OLD/samsung_tv_token.state.json"        "$DATA/tv/samsung_tv_token.state.json"
mv_if "$OLD/tv_weather_art"                     "$DATA/tv/tv_weather_art"
mv_if "$OLD/public/noticias"                    "$DATA/public/noticias"
mv_if "$OLD/whatsapp_autoresponder_state.json"  "$DATA/whatsapp/whatsapp_autoresponder_state.json"
mv_if "$OLD/bench"                              "$DATA/bench"
rmdir "$OLD/public" 2>/dev/null || true
rmdir "$OLD" || { log "left in $OLD:"; ls -la "$OLD"; exit 1; }

# ── newsletter output ────────────────────────────────────────────────────────
mv_if /home/julio/Documents/X_posts "$DATA/newsletter"

# ── WhatsApp bridges ─────────────────────────────────────────────────────────
mv_if /home/julio/whatsapp-bridge/store       "$DATA/whatsapp/store"
mv_if /home/julio/whatsapp-bridge/store-funes "$DATA/whatsapp/store-funes"
cp -p /home/julio/whatsapp-bridge/whatsapp-bridge "$FUNES/third-party/whatsapp-mcp/whatsapp-bridge/whatsapp-bridge"
mv /home/julio/whatsapp-bridge "/home/julio/whatsapp-bridge.retired-$STAMP"

# ── funes-julio's untracked runtime pieces ───────────────────────────────────
J=/home/julio/funes-julio
mv_if "$J/scriptlib/.venv"                                       "$FUNES/scriptlib/.venv"
mv_if "$J/third-party/imap-email-mcp-patched/node_modules"       "$FUNES/third-party/imap-email-mcp-patched/node_modules"
mv_if "$J/third-party/whatsapp-mcp/whatsapp-mcp-server/.venv"    "$FUNES/third-party/whatsapp-mcp/whatsapp-mcp-server/.venv"
mv_if "$J/agents/alphaxiv-researcher.md"                         "$FUNES/config/backups/alphaxiv-researcher.md"
for f in "$J"/config/funes.local.bak-*; do mv_if "$f" "$FUNES/config/backups/from-funes-julio/$(basename "$f")"; done
for f in "$FUNES"/config/funes.local.bak-*; do mv_if "$f" "$FUNES/config/backups/$(basename "$f")"; done
mv "$J" "/home/julio/funes-julio.retired-$STAMP"

# ── funes.local ──────────────────────────────────────────────────────────────
L=$FUNES/config/funes.local
cp -p "$L" "$FUNES/config/backups/funes.local.bak-$STAMP-premerge"
sed -i \
    -e 's|^FUNES_DB=.*|FUNES_DB=/home/julio/Funes/data/memory.db|' \
    -e 's|^FUNES_WORKSPACE_DIR=.*|FUNES_WORKSPACE_DIR=/home/julio/Funes/data/workspace|' \
    -e 's|^WHATSAPP_STATE_PATH=.*|WHATSAPP_STATE_PATH=/home/julio/Funes/data/whatsapp/whatsapp_autoresponder_state.json|' \
    -e 's|^WHATSAPP_DB_PATH=.*|WHATSAPP_DB_PATH=/home/julio/Funes/data/whatsapp/store-funes/messages.db|' \
    -e 's|^FUNES_TV_TOKEN_FILE=.*|FUNES_TV_TOKEN_FILE=/home/julio/Funes/data/tv/samsung_tv_token|' \
    -e 's|^FUNES_NEWS_FEED_DIR=.*|FUNES_NEWS_FEED_DIR=/home/julio/Funes/data/public/noticias|' \
    "$L"
if grep -nE '\.funes-v4|funes-julio|X_posts|/whatsapp-bridge/' "$L" | grep -v '^[0-9]*:#'; then
    log "funes.local still names an old path (above) — fix by hand"; exit 1
fi

# ── crontab ──────────────────────────────────────────────────────────────────
crontab -l \
  | sed -e 's|/home/julio/funes-julio/|/home/julio/Funes/|g' \
        -e 's|/home/julio/Documents/X_posts|/home/julio/Funes/data/newsletter|g' \
  | crontab -

# ── systemd ──────────────────────────────────────────────────────────────────
U=/home/julio/.config/systemd/user
systemctl --user disable funes-v4 whatsapp-autoresponder-v4
mkdir -p "$FUNES/config/backups/systemd"
mv "$U/funes-v4.service" "$U/whatsapp-autoresponder-v4.service" "$FUNES/config/backups/systemd/"
cp "$FUNES"/scripts/{funes,whatsapp-autoresponder,whatsapp-bridge,whatsapp-bridge-funes}.service "$U/"
systemctl --user daemon-reload
systemctl --user enable --now whatsapp-bridge whatsapp-bridge-funes funes
systemctl --user enable --now whatsapp-autoresponder

# ── tailscale funnel ─────────────────────────────────────────────────────────
tailscale serve --bg --set-path /noticias "$DATA/public/noticias" \
    || log "could not repoint the funnel — run: tailscale serve --bg --set-path /noticias $DATA/public/noticias"

log "done. backup: $BACKUP"
