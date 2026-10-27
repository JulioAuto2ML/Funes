#!/usr/bin/env bash
# =============================================================================
# scripts/funesctl.sh — start/stop/restart/status for the Funes systemd --user
# service (see the [Install]/[Unit] block in the deploy notes for the unit
# file itself; this just wraps `systemctl --user` for the funes.service name).
# =============================================================================
set -euo pipefail

SERVICE=funes.service

usage() {
    echo "Usage: $0 {start|stop|restart|status}" >&2
    exit 1
}

[ $# -eq 1 ] || usage

case "$1" in
    start|stop|restart)
        systemctl --user "$1" "$SERVICE"
        systemctl --user status "$SERVICE" --no-pager
        ;;
    status)
        systemctl --user status "$SERVICE" --no-pager
        ;;
    *)
        usage
        ;;
esac
