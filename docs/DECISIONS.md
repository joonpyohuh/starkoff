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
