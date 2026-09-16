#!/usr/bin/env bash
# Archive the workspace this script was started in.
#
# Arguments arrive as `--name=value` tokens — see scriptlib/README.md. Funes
# validates them against the manifest before this runs, but the alphabet check
# below stays: a script in this library is meant to be safe to run on its own
# terms, not safe only because of who called it.
set -euo pipefail

archive_name=""
for token in "$@"; do
    case "$token" in
        --name=*) archive_name="${token#--name=}" ;;
        *) echo "unexpected argument: $token" >&2; exit 2 ;;
    esac
done

if [[ -z "$archive_name" ]]; then
    archive_name="$(date -u +%Y%m%d-%H%M%S)"
elif [[ ! "$archive_name" =~ ^[A-Za-z0-9_-]+$ ]]; then
    echo "name must be letters, digits, '-' or '_' (got: $archive_name)" >&2
    exit 2
fi

mkdir -p backups
target="backups/${archive_name}.tar.gz"
if [[ -e "$target" ]]; then
    echo "refusing to overwrite an existing archive: $target" >&2
    exit 1
fi

# --exclude=./backups: a backup of the backups doubles the directory every run.
tar --exclude=./backups -czf "$target" . 2>/dev/null
echo "wrote $target ($(du -h "$target" | cut -f1))"
