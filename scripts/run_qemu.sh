#!/usr/bin/env bash
# Run the built firmware in QEMU. UART shell is on this terminal.
# Host port 7000 -> firmware control port. Firmware reaches the host at 10.0.2.2.
# Exit QEMU with: Ctrl-a x
set -euo pipefail
source "$(dirname "$0")/env.sh"
west build -d "$BUILD_DIR" -t run
