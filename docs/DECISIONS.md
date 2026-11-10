# Design decisions

Each entry: what was decided, why, and what was given up. Newest at the bottom.

## D1. Zephyr RTOS v4.2.0 on `qemu_x86`
- **Why**: The assignment requires an MCU-class environment, not a Linux process. Zephyr is a real RTOS
  used in products, it ships a TCP/IP stack, a WebSocket client and a shell, and `qemu_x86` has an
  emulated e1000 Ethernet card, so the firmware can reach a real server without extra hardware models.
- **Given up**: A Cortex-M board (e.g. `mps2/an385`) would look more like a real charger MCU, but its
  QEMU networking setup is more fragile. x86 is still a bare-metal RTOS image, not Linux.

## D2. West manifest repository (T2) with a module allowlist
- **Why**: The grader builds on a clean Ubuntu 22.04. `west.yml` pins Zephyr to the tag `v4.2.0`, so the
  grader gets exactly the tree we tested. `name-allowlist` limits downloads to `mbedtls` (needed by the
  WebSocket handshake) and `picolibc`, instead of ~60 HAL modules.

## D3. QEMU user-mode networking (SLIRP) + e1000
- **Why**: No root, no TAP/bridge setup on the grader's machine. Inside QEMU the host is `10.0.2.2`.
  The firmware's TCP control port is exposed to the host with `hostfwd`.
- **Given up**: The host cannot open arbitrary connections into the guest, only forwarded ports. That is
  fine because the charger is always the WebSocket *client* in OCPP-J.

## D4. Web UI is a single `index.html` + one JS + one CSS (no npm, no React)
- **Why**: The UI is only a test/demo tool. A build step (Node, npm) would add another thing that can
  break on the grader's machine. All test logic stays in Python so the terminal and the browser give
  the same results.

## D5. ChangeAvailability is part of the MVP
- **Why**: TC_013_CS and TC_014_CS (Reset) start with `ChangeAvailability(Inoperative)` and end with
  `Operative`. Without it, Reset cannot pass these tests even if Reset itself works.

## D6. Mock CSMS can script its responses per test
- **Why**: TC_001_CS first answers BootNotification with `Rejected`, then `Accepted`. The CSMS must be
  able to change answers per test, otherwise the retry path is never exercised.

## D7. Static IP 10.0.2.15 + "link announce" (not DHCP)
- **Finding**: Zephyr v4.2.0's e1000 driver enables receive with `RCTL_EN | RCTL_MPE` but without
  `RCTL_BAM` (accept broadcast), so QEMU drops every broadcast frame to the guest. DHCP never completes,
  and SLIRP's ARP requests for 10.0.2.15 never arrive, so the host->guest port forward (control port)
  silently hangs until the guest happens to send something first.
- **Decision**: static IP (SLIRP always uses 10.0.2.15 anyway) and `src/net/link.c` sends one tiny UDP
  datagram to the gateway at boot and every 30 s. Our own ARP request teaches SLIRP our MAC; its reply is
  unicast, so it arrives. Zephyr itself is not patched (the grader gets plain upstream v4.2.0).
- **How it was found**: control port timed out; gdb/`net conn` showed no SYN ever reached the TCP stack;
  after a manual `net ping` it worked; DHCP also timed out; then read the driver's RCTL bits.

## D8. Default server URL / CP ID are Kconfig options, set by `scripts/build.sh --url --id`
- **Why**: A build-time default is the one path that works on any machine, with no storage involved.
  Runtime change via the shell is added on top (see later decisions).

## D9. Persistent storage: ivshmem on qemu_x86 (NVMe and qemu_x86_64 rejected)
- **Problem**: `qemu_x86`'s simulated flash is RAM and is erased on every boot; `__noinit` RAM is also
  cleared on reboot (tested: boot counter always 1).
- **Tried**: NVMe disk. Does not build on 32-bit x86 (MSI helpers exist only for 64-bit). On `qemu_x86_64`
  NVMe persisted across reboot and QEMU restart (tested), and the owner chose to switch boards for it,
  but e1000 networking on x86_64 then did not work in our tests, so that path was dropped.
- **Decision**: stay on `qemu_x86` and use QEMU `ivshmem-plain`: a 1 MiB host file mapped as a PCI
  memory BAR. The firmware treats it like memory-mapped NOR flash. Tested: survives in-QEMU reboot and
  QEMU restart. On a real charger this layer would be flash/FRAM; only `src/store/store.c` would change.
- **Power-loss safety**: two slots (A/B) per record, sequence number + CRC32, header written last.
  A write cut in the middle leaves the older slot valid.

## D10. A new build's default URL / CP id win over stored ones
- **Why**: otherwise a grader who rebuilds with `--url/--id` but keeps an old `state/cp_nvm.img` would
  silently connect to the old server. The build stores a tag `url|id`; if it differs at boot, the stored
  URL/id are dropped. Other keys (HeartbeatInterval etc.) are kept. Tested.

## D11. One command parser for both the UART shell and the TCP control port
- **Why**: the web UI/tests (TCP) and a human (UART: `cp plug 1`) must get exactly the same behaviour.
  `ctrl_exec()` is shared; the transports only move lines.

## D12. The hardware simulator keeps running while "power is off"
- **Why**: the sim plays the outside world (mains, cable, car). Someone must be able to switch power back
  on. Only the charger logic stops. Documented as a simulation limit.

## D13. Readonly keys are readonly for everyone
- Even the local shell cannot change `GetConfigurationMaxKeys`, `NumberOfConnectors`,
  `SupportedFeatureProfiles`; their values always come from the build.

## D14. Control port clients must not half-close; `scripts/ctl.sh` provided
- **Finding**: `printf 'status\n' | nc -q 1 ...` got no reply. Debug log: `recv` got the data, then
  `send` failed with `ENOTCONN` (errno 128). The client's FIN arrived with the data, and Zephyr's TCP
  does not support half-close, so the connection is gone before we can answer.
- **Decision**: not worked around in firmware (it is a Zephyr TCP property). `scripts/ctl.sh` sends
  commands and waits for each reply; interactive `nc` works. The lab tools keep one connection open.
  OCPP is not affected (the firmware is the WebSocket client and the connection stays open).
