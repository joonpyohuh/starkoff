#!/usr/bin/env bash
# Build the firmware for qemu_x86.
#   scripts/build.sh [--url ws://HOST:PORT/path] [--id CP_ID] [--pristine]
# --url / --id set the DEFAULT values baked into the image (Kconfig).
set -euo pipefail
source "$(dirname "$0")/env.sh"
EXTRA=() ; PRISTINE=auto
while [ $# -gt 0 ]; do
  case "$1" in
    --url) EXTRA+=("-DCONFIG_OCPP_SERVER_URL=\"$2\""); shift 2 ;;
    --id)  EXTRA+=("-DCONFIG_OCPP_CHARGE_POINT_ID=\"$2\""); shift 2 ;;
    --pristine) PRISTINE=always; shift ;;
    *) echo "unknown option $1"; exit 1 ;;
  esac
done
west build -p "$PRISTINE" -b qemu_x86 -d "$BUILD_DIR" "$REPO_DIR/firmware" -- "${EXTRA[@]}"
