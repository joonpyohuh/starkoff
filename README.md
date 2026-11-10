# STARKOFF take-home: OCPP 1.6J Charge Point firmware on QEMU

Charge point firmware for OCPP 1.6J (JSON over WebSocket), running on **Zephyr RTOS v4.2.0** on the
QEMU board **`qemu_x86`** (an MCU-class RTOS image, not a Linux program). The real-world parts of a
charger (cable, EV, RFID card, contactor, lock, meter, mains power, network link, sensors) are
simulated inside the firmware and can be driven from a UART shell or a TCP control port.

> **Status: work in progress (2026-11-10).** OCPP messaging is not implemented yet.
> What works today is listed in [Current status](#current-status). Nothing below is claimed unless it
> was run and observed. The clean-machine procedure below has **not yet been verified on a fresh
> Ubuntu 22.04 install**; that check is planned before submission.

---

## 1. Build and run on a clean Ubuntu 22.04

### 1.1 System packages
```bash
sudo apt update
sudo apt install --no-install-recommends -y git cmake ninja-build gperf ccache dfu-util \
  device-tree-compiler wget python3-dev python3-pip python3-setuptools python3-venv \
  python3-wheel xz-utils file make gcc libmagic1 netcat-openbsd
```

### 1.2 Workspace (west manifest repository)
This repository is a west manifest. `west update` fetches Zephyr **v4.2.0** and only the modules
the firmware needs (`mbedtls`, `picolibc`).
```bash
mkdir -p ~/starkoff-ws && cd ~/starkoff-ws
python3 -m venv .venv && source .venv/bin/activate
pip install west
git clone https://github.com/joonpyohuh/starkoff.git starkoff
west init -l starkoff
west update --narrow -o=--depth=1
west zephyr-export
pip install -r zephyr/scripts/requirements-base.txt
west sdk install --version 0.17.2 -t x86_64-zephyr-elf   # toolchain + host tools (includes QEMU)
```
Resulting layout (the scripts rely on it):
```
~/starkoff-ws/
  .venv/      starkoff/ (this repo)      zephyr/      modules/
```

### 1.3 Build
```bash
cd ~/starkoff-ws/starkoff
scripts/build.sh                                   # defaults: ws://10.0.2.2:9000/ocpp, CP001
scripts/build.sh --url ws://10.0.2.2:9000/ocpp --id CP001   # set the defaults explicitly
```
The firmware connects to `<url>/<ChargePointId>`. Inside QEMU the host machine is `10.0.2.2`.

### 1.4 Run
```bash
scripts/run_qemu.sh --fresh       # --fresh = start from factory settings
```
- This terminal is the firmware's **UART shell** (prompt `uart:~$`). Quit QEMU: `Ctrl-a` then `x`.
- Host `127.0.0.1:7000` is forwarded to the firmware's **TCP control port**.
- Settings are kept in `state/cp_nvm.img` (non-volatile memory, see below). Without `--fresh` they
  survive reboots and QEMU restarts.

---

## 2. Changing the server address and Charge Point ID

| Way | How | Persists? |
|---|---|---|
| Build-time default | `scripts/build.sh --url <ws://host:port/path> --id <CP_ID>` | yes. A new build's values replace values stored by an older build |
| At runtime, UART shell | `cp cfg set _ServerUrl ws://10.0.2.2:9000/ocpp` / `cp cfg set _ChargePointId CP001` then `cp reboot` | yes |
| At runtime, TCP port 7000 | `cfg set _ServerUrl ...` / `cfg set _ChargePointId ...` then `reboot` | yes |

(Storing and persisting these values is verified. The OCPP connection that uses them is not
implemented yet.)

---

## 3. Simulated hardware: control commands

Same commands on the UART shell (prefix `cp `) and on TCP port 7000 (one line in, one line out):

| Command | Meaning |
|---|---|
| `status` | one line of `key=value` (power, LED, link, cable, lock, relay, energy, faults) |
| `plug <c>` / `unplug <c>` | cable in / out (charger and EV side). `unplug` is refused while the connector is locked |
| `ev_disconnect <c>` | cable pulled out on the EV side only |
| `card <c> <idTag>` | present an RFID card |
| `power off` / `power on` | mains power cut / restored |
| `lock_fail <c> on\|off` | make the connector lock motor fail |
| `fault <c> overcurrent\|overtemp\|overvoltage\|off` | push a sensor out of its safe range (detected within 1 s) |
| `net_down` / `net_up` | simulate pulling / reconnecting the network cable |
| `cfg get <k>` / `cfg set <k> <v>` / `cfg list` / `cfg reset` | configuration keys |
| `reboot` | reboot the firmware |

Examples:
```bash
scripts/ctl.sh status                          # one-shot, prints each reply
scripts/ctl.sh "plug 1" "card 1 TAG001" status

nc 127.0.0.1 7000                              # interactive: type a command, get a reply
```
Do **not** use `nc -q`/`nc -N` or `echo cmd | nc ...`: they half-close the connection, and Zephyr's TCP
then drops the replies (Zephyr does not support TCP half-close).

---

## 4. Current status

| Area | State | How it was checked |
|---|---|---|
| Firmware boots on QEMU `qemu_x86`, e1000 + user-mode network | works | ran it; TCP connect from firmware to host |
| TCP control port 7000 + UART shell commands | works | ran all commands above; replies as expected; port answers ~2.5 s after QEMU start |
| Config keys with validation (int/bool/measurand lists, readonly keys) | works | `cfg set` with valid and invalid values |
| Settings persist across reboot and QEMU restart | works | changed a key, `reboot`, restarted QEMU, value still there |
| Simulated sensors: over-temperature detected as a fault | works | `fault 1 overtemp` → `c1.fault=HighTemperature` |
| OCPP 1.6J (BootNotification, transactions, ...) | **not implemented yet** | — |
| Mock CSMS, web UI, test runner | **not implemented yet** | — |
| Clean Ubuntu 22.04 procedure (section 1) | **not verified yet** | planned |

Detailed OCTT test results will be in `docs/TEST_RESULTS.md`.

---

## 5. Known limits (so far)
- **Non-volatile memory is a host file** mapped into the firmware with QEMU's `ivshmem` device
  (`qemu_x86` has no persistent flash). On real hardware this would be flash/FRAM; only
  `firmware/src/store/store.c` would change. See [docs/DECISIONS.md](docs/DECISIONS.md) D9.
- **Zephyr v4.2.0's e1000 driver does not accept broadcast frames**, so DHCP cannot work in QEMU. The
  firmware uses the static address 10.0.2.15 and sends a small packet to the gateway at boot and every
  30 s so QEMU's NAT can reach the control port (D7).
- QEMU prints `Slirp: Failed to send packet, ret: -1` now and then. That is QEMU reporting a frame
  the e1000 driver did not accept (broadcasts, see above, or a frame arriving while its single receive
  buffer is busy). TCP retransmits; it is not a firmware crash.
- The hardware simulator keeps running while "power is off" (it is the outside world); only the
  charger logic stops (D12).

---

## 6. What I built, and how AI was used
- **Me (the author)**: requirements, scope and test-case priorities, the offline / power-loss / fault
  design ([DESIGN_resilience.md](DESIGN_resilience.md)), design decisions, reviewing every step.
- **AI (Claude Code)**: wrote most of the code and docs under my direction, ran builds and QEMU tests,
  and debugged problems. The step-by-step record is in [docs/AI_USAGE.md](docs/AI_USAGE.md).
- Design decisions and their reasons: [docs/DECISIONS.md](docs/DECISIONS.md).

## 7. Repository layout
```
firmware/   Zephyr app (C) — the deliverable
  src/ocpp/   OCPP logic, configuration keys
  src/net/    networking helpers (WebSocket comes next)
  src/sim/    simulated hardware
  src/ctrl/   UART shell + TCP control port
  src/store/  non-volatile record storage
lab/        host tools (mock CSMS, web UI, tests) — not part of the firmware
scripts/    build.sh, run_qemu.sh
docs/       DECISIONS.md, AI_USAGE.md, reference/ (spec, OCTT test cases, assignment)
west.yml    pins Zephyr v4.2.0 and the module allowlist
```

## 8. Third-party
- Zephyr RTOS v4.2.0 (Apache-2.0), mbedtls, picolibc — fetched by `west update`, not copied here.
- `docs/reference/`: OCPP 1.6 Edition 2 specification and OCTT test case document © Open Charge
  Alliance, included unmodified for reference.
