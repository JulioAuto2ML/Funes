#!/usr/bin/env bash
# Runs tv_art.py with this directory's own virtualenv — samsungtvws is not
# something Funes or the system Python should carry. See README.md for setup.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py="$here/.venv/bin/python"
if [[ ! -x "$py" ]]; then
    echo "tv_art: $here/.venv is missing — run: python3 -m venv $here/.venv && $here/.venv/bin/pip install -r $here/requirements.txt" >&2
    exit 3
fi
exec "$py" "$here/tv_art.py" "$@"
