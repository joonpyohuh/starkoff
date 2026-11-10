# Sourced by the other scripts. Finds the west workspace (ZEPHYR_BASE) and the west binary.
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

# A west workspace is a directory with .west/config; its [manifest] path names this repo.
# Returns the workspace dir if its manifest path resolves to REPO_DIR.
_ws_for_repo() {
  local ws="$1" mpath
  [ -f "$ws/.west/config" ] || return 1
  mpath=$(sed -n '/^\[manifest\]/,/^\[/{s/^path *= *//p}' "$ws/.west/config" | head -1)
  [ -n "$mpath" ] && [ "$(readlink -f "$ws/$mpath")" = "$REPO_DIR" ] && echo "$ws"
}

if [ -z "${ZEPHYR_BASE:-}" ]; then
  WS=""
  # 1) README layout: <workspace>/<this repo>, i.e. the parent directory is the workspace.
  WS=$(_ws_for_repo "$REPO_DIR/..") || WS=""
  # 2) Repo kept elsewhere and linked into a workspace (e.g. <ws>/starkoff -> this repo).
  if [ -z "$WS" ]; then
    for cfg in "$HOME"/*/.west/config; do
      [ -f "$cfg" ] || continue
      WS=$(_ws_for_repo "$(dirname "$(dirname "$cfg")")") && break
      WS=""
    done
  fi
  if [ -n "$WS" ] && [ -d "$WS/zephyr" ]; then
    export ZEPHYR_BASE="$(cd "$WS/zephyr" && pwd)"
  else
    echo "[env] ERROR: no west workspace found for $REPO_DIR." >&2
    echo "[env] Follow README section 1.2, or set ZEPHYR_BASE=<workspace>/zephyr." >&2
    exit 1
  fi
fi
WS_DIR="$(cd "$ZEPHYR_BASE/.." && pwd)"

if ! command -v west >/dev/null 2>&1; then
  # README layout puts the virtualenv in <workspace>/.venv
  for v in "$WS_DIR/.venv/bin" "$REPO_DIR/../.venv/bin" "$HOME/zephyrproject/.venv/bin"; do
    [ -x "$v/west" ] && export PATH="$v:$PATH" && break
  done
fi
command -v west >/dev/null 2>&1 || {
  echo "[env] ERROR: 'west' not found. Activate the virtualenv: source <workspace>/.venv/bin/activate" >&2
  exit 1
}
BUILD_DIR="$REPO_DIR/build"
