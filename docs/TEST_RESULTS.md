# Test results

This file is the single place that says what works, what does not, and what was not tested.
A result is only listed as PASS if it was run against the real firmware in QEMU.

## How these results were produced
- Firmware commit `d2e9104`, Zephyr v4.2.0, board `qemu_x86`, run on the developer PC (WSL2 Ubuntu 22.04).
- Command: `scripts/run_lab.sh --qemu --all --markdown` (starts the firmware fresh, then runs each test
  against our **own mock CSMS** in `lab/csms/`). This is **not** the official OCTT tool; each scenario
  follows the steps and validations of the OCTT document (`docs/reference/testcases.txt`).
- Date: 2026-11-10.

## Latest run
| Test case | Name | Result | Details |
|---|---|---|---|
| TC_001_CS | Cold Boot Charge Point | PASS | re-boot after 10.0 s (Rejected interval 10 s); heartbeat gap 10.0 s (interval 10 s) (49 s) |
| TC_002_CS | Cold Boot Charge Point - Pending | PASS | heartbeat gap 10.0 s (interval 10 s) (47 s) |
| TC_019_CS | Retrieve configuration | PASS | 25 keys, GetConfigurationMaxKeys=50 (1 s) |
| TC_021_CS | Change/set Configuration | PASS |  (1 s) |
| X_NOTIMPL | Unknown action -> CALLERROR NotImplemented | PASS |  (1 s) |
| X_RECONNECT | Connection loss and reconnect without reboot | PASS | reconnected, first message StatusNotification(connectorId=0) (5 s) |
| X_REJECTED_SILENT | While Rejected the CP answers nothing | PASS | no answer while Rejected, BootNotification retried after the interval (32 s) |

`X_...` ids are extra lab checks, not OCTT test cases (see DECISIONS D19).

## Planned but not implemented yet (expected to FAIL today)
| Test case | Needs |
|---|---|
| TC_003_CS, TC_004_1_CS, TC_068_CS | Authorize, Start/StopTransaction |
| TC_010_CS, TC_011_1_CS, TC_012_CS | RemoteStart/RemoteStopTransaction |
| TC_013_CS, TC_014_CS | ChangeAvailability, Reset |
| TC_017_1_CS | UnlockConnector |
| everything else in the OCTT document | not started |

## Not applicable
| Test case | Why |
|---|---|
| TC_017_2_CS, TC_018_2_CS | the simulated charge point has a detachable (not fixed) cable |
