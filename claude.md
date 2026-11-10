# CLAUDE.md — STARKOFF OCPP 1.6J Charge Point on QEMU

Take-home assignment. Deadline: 2026-11-16 12:00 KST (default branch is graded).
The owner is new to C/embedded. Explain every design decision in plain Korean, briefly.

## Hard constraints (from the assignment)
- Firmware runs on QEMU as an MCU-class target (Zephyr v4.2.0, board `qemu_x86`). Not a Linux program.
- Server URL and Charge Point ID must be changeable.
- Must build and run on a clean Ubuntu 22.04 following README only.
- Never claim a feature works unless it was actually run and observed. Unverified = say "not verified".
- Record AI usage in `docs/AI_USAGE.md`, design decisions in `docs/DECISIONS.md`.

## Repository layout (keep the two worlds separate)
```
firmware/   Zephyr app (C). The real deliverable. Runs inside QEMU.
  src/ocpp/     OCPP message handling, transactions, config keys
  src/net/      WebSocket connection, reconnect, outgoing queue
  src/sim/      Fake hardware: cable, card, EV, power, meter, lock
  src/ctrl/     Control interface (UART shell + TCP control port)
lab/        Host-side tools (Python). Only for testing and demo. NOT part of firmware.
  csms/         Minimal OCPP 1.6J central system (WebSocket server)
  web/          Browser UI: car, charger, draggable card, message log, test buttons
  tests/        Test case scenarios (one file per TC group)
scripts/    build.sh, run_qemu.sh, run_lab.sh
docs/       reference/ (original PDFs + extracted .txt), DECISIONS.md, AI_USAGE.md, TEST_RESULTS.md
west.yml    Pins zephyr v4.2.0; only needed modules via name-allowlist
```
Rules:
- `firmware/` must never depend on anything in `lab/`.
- `lab/` talks to firmware only through (1) OCPP WebSocket as the CSMS and (2) the TCP control port.
- The web UI is a thin view. All test logic lives in `lab/tests/`, so the same tests run from the terminal.

## Control port protocol (firmware <-> lab, plain text, one line per command)
`plug <c>` `unplug <c>` `card <c> <idTag>` `ev_disconnect <c>` `power off|on`
`lock_fail <c> on|off` `status` -> firmware replies with one line of `key=value` pairs.
Keep it this small. Add a command only when a test needs it.

## Token budget rules
- Do NOT read the PDFs directly. Use the extracted text and grep:
  - `docs/reference/spec.txt` (OCPP 1.6 Edition 2), `docs/reference/testcases.txt` (OCTT)
  - Find a test: `grep -n "TC_010_CS" docs/reference/testcases.txt` then read only ~60 lines around it.
  - Find a message: `grep -n "RemoteStartTransaction" docs/reference/spec.txt` then read only the relevant section.
- Do not re-read a file you already read in this session unless it changed.
- Do not print whole files back to the user. Show only the changed part or a short summary.
- Keep replies short: what changed, how it was checked, what is next.
- Prefer editing existing files over creating new ones.

## Verification rules (light but honest)
- After a change, run ONLY the related test(s): `scripts/run_lab.sh --test TC_xxx_CS`.
- Run the full test set only when asked, or once at the end of a work session.
- Build firmware only when firmware code changed. Lab-only changes do not need a firmware rebuild.
- When a test fails, report: which step, what was expected, what was received. Then fix or mark it.
- `docs/TEST_RESULTS.md` is the single source of truth for "works / doesn't work / not tested".

## Coding conventions
- C: small functions, no dynamic allocation in hot paths, fixed-size buffers with bounds checks.
- Every OCPP CALL gets a CALLRESULT or CALLERROR. Unknown action -> CALLERROR NotImplemented.
- Log one line per OCPP message in/out with a short prefix (`>>` sent, `<<` received).
- Python: standard library + `websockets` (and `aiohttp` only if needed). No frameworks.
- Web: single `index.html` + one JS file + one CSS file. No build step, no npm.

## Work style
- Before a big step, show a short plan (5 lines max) and wait for approval.
- README.md section 1 is the clean-Ubuntu reproduction procedure. Whenever a step adds a dependency,
  script, flag or path, update it in the same commit. Never rely on paths that exist only on the dev PC
  (e.g. a hard-coded ZEPHYR_BASE).
- Commit in small units with clear messages. Append a line to `docs/AI_USAGE.md` per meaningful AI-written part.