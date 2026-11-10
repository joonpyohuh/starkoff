#!/usr/bin/env bash
# Run the lab tools. Examples:
#   scripts/run_lab.sh --list
#   scripts/run_lab.sh --qemu --test TC_001_CS      # start firmware in QEMU, run one test
#   scripts/run_lab.sh --qemu --all --markdown      # all tests, README table
#   scripts/run_lab.sh --csms                       # only the mock CSMS (firmware started by you)
set -euo pipefail
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd -P)"
PY="$REPO_DIR/lab/.venv/bin/python"
[ -x "$PY" ] || { echo "Run scripts/setup_lab.sh first."; exit 1; }
cd "$REPO_DIR"
exec "$PY" -m lab.run "$@"
