#!/usr/bin/env bash
# Run the built firmware in QEMU (board qemu_x86).
#   scripts/run_qemu.sh [--ctrl-port 7000] [--disk state/cp_nvm.img] [--fresh]
#
# - UART shell is on this terminal. Exit QEMU with: Ctrl-a x
# - Network: QEMU user mode (SLIRP) + e1000. The firmware reaches the host at 10.0.2.2.
# - Host TCP port (default 7000, bound to 127.0.0.1) -> firmware control port 7000.
# - Non-volatile memory: a 1 MiB host file mapped into the firmware via ivshmem.
#   It keeps settings across reboots and QEMU restarts. --fresh deletes it first.
set -euo pipefail
source "$(dirname "$0")/env.sh"
CTRL_PORT=7000
DISK="$REPO_DIR/state/cp_nvm.img"
FRESH=0
while [ $# -gt 0 ]; do
  case "$1" in
    --ctrl-port) CTRL_PORT="$2"; shift 2 ;;
    --disk) DISK="$2"; shift 2 ;;
    --fresh) FRESH=1; shift ;;
    *) echo "unknown option $1"; exit 1 ;;
  esac
done
ELF="$BUILD_DIR/zephyr/zephyr.elf"
[ -f "$ELF" ] || { echo "Not built yet. Run scripts/build.sh first."; exit 1; }
QEMU=$(sed -n 's/^QEMU:FILEPATH=//p' "$BUILD_DIR/CMakeCache.txt")
[ -x "$QEMU" ] || QEMU=$(command -v qemu-system-i386)
mkdir -p "$(dirname "$DISK")"
[ "$FRESH" = 1 ] && rm -f "$DISK"
[ -f "$DISK" ] || truncate -s 1M "$DISK"
echo "[run_qemu] control port: 127.0.0.1:$CTRL_PORT   nvm: $DISK   (exit: Ctrl-a x)"
exec "$QEMU" -m 32 -cpu qemu32,+nx,+pae -machine q35 -machine acpi=off -nographic \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  -chardev stdio,id=con,mux=on -serial chardev:con -mon chardev=con,mode=readline \
  -nic user,model=e1000,hostfwd=tcp:127.0.0.1:"$CTRL_PORT"-:7000 \
  -object memory-backend-file,id=nvm,share=on,mem-path="$DISK",size=1M \
  -device ivshmem-plain,memdev=nvm \
  -kernel "$ELF"
