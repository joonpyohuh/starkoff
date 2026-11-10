#!/usr/bin/env bash
# Send control commands to the running firmware and print each reply.
#   scripts/ctl.sh status
#   scripts/ctl.sh "plug 1" "card 1 TAG001" status
#   CTRL_PORT=7001 scripts/ctl.sh status
# Waits for every reply before closing. (Zephyr's TCP does not support half-close,
# so tools like "nc -q" that close their sending side early get no replies.)
exec python3 - "${CTRL_PORT:-7000}" "$@" <<'PY'
import socket, sys
port, cmds = int(sys.argv[1]), sys.argv[2:] or ["status"]
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
except OSError as e:
    sys.exit(f"cannot connect to 127.0.0.1:{port} ({e}). Is scripts/run_qemu.sh running?")
f = s.makefile("rw")
for c in cmds:
    f.write(c + "\n"); f.flush()
    line = f.readline()
    if not line:
        sys.exit(f"{c}: connection closed")
    print(f"{c:<20} -> {line.rstrip()}")
PY
