#!/usr/bin/env bash
# Creates benchmarks/knowledge-offload/.venv — separate from every other
# environment on purpose (E1b adds ROCm torch here, never into ComfyUI's venv).
set -euo pipefail
cd "$(dirname "$0")"
python3 -m venv .venv
# The harness needs nothing from PyPI; pytest is only for tests/. If the index
# is unreachable the venv is still usable and tests run with any pytest.
.venv/bin/pip install -q -r requirements.txt || echo "warning: pip failed; harness runs, tests need a pytest"
echo "ok: $(.venv/bin/python --version) in $(pwd)/.venv"
