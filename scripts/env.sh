# Sourced by the other scripts. Finds the west workspace and the west binary.
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -z "${ZEPHYR_BASE:-}" ]; then
  # Standard layout from the README: <workspace>/<this repo> and <workspace>/zephyr
  if [ -d "$REPO_DIR/../zephyr" ]; then
    export ZEPHYR_BASE="$(cd "$REPO_DIR/../zephyr" && pwd)"
  fi
fi
if ! command -v west >/dev/null 2>&1; then
  for v in "$REPO_DIR/../.venv/bin" "$HOME/zephyrproject/.venv/bin"; do
    [ -x "$v/west" ] && export PATH="$v:$PATH" && break
  done
fi
BUILD_DIR="$REPO_DIR/build"
