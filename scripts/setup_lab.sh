#!/usr/bin/env bash
# Create the Python virtualenv for the lab tools (mock CSMS, tests, web UI).
set -euo pipefail
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd -P)"
python3 -m venv "$REPO_DIR/lab/.venv"
"$REPO_DIR/lab/.venv/bin/pip" install -q --upgrade pip
"$REPO_DIR/lab/.venv/bin/pip" install -q -r "$REPO_DIR/lab/requirements.txt"
echo "lab virtualenv ready: $REPO_DIR/lab/.venv"
