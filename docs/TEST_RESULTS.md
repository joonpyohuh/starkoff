# 시험 결과

무엇이 되고, 무엇이 안 되고, 무엇을 시험하지 않았는지를 적는 단 하나의 문서입니다.
QEMU에서 실제 펌웨어로 실행한 것만 PASS로 적습니다.

## 결과를 만든 방법
- 펌웨어 커밋 `3533f29`, Zephyr v4.2.0, 보드 `qemu_x86`, 개발 PC(WSL2 Ubuntu 22.04)에서 실행.
- 명령: `scripts/run_lab.sh --qemu --all --markdown`. 펌웨어를 초기 상태로 띄운 뒤 **우리가 만든
  가짜 CSMS**(`lab/csms/`)를 상대로 각 시험을 실행합니다. 공식 OCTT 도구가 **아닙니다.** 각 시나리오는
  OCTT 문서(`docs/reference/testcases.txt`)의 단계와 검증 항목을 그대로 따릅니다.
- 날짜: 2026-11-10. 실행 중 펌웨어 크래시(FATAL) 0건.

## 최근 실행
| Test case | Name | Result | Details |
|---|---|---|---|
| TC_001_CS | Cold Boot Charge Point | PASS | re-boot after 10.0 s (Rejected interval 10 s); heartbeat gap 10.0 s (interval 10 s) (37 s) |
| TC_002_CS | Cold Boot Charge Point - Pending | PASS | heartbeat gap 10.0 s (interval 10 s) (41 s) |
| TC_019_CS | Retrieve configuration | PASS | 25 keys, GetConfigurationMaxKeys=50 (1 s) |
| TC_021_CS | Change/set Configuration | PASS |  (1 s) |
| X_NOTIMPL | Unknown action -> CALLERROR NotImplemented | PASS |  (1 s) |
| X_RECONNECT | Connection loss and reconnect without reboot | PASS | reconnected, first message StatusNotification(connectorId=0) (7 s) |
| X_REJECTED_SILENT | While Rejected the CP answers nothing | PASS | no answer while Rejected, BootNotification retried after the interval (26 s) |

`X_`로 시작하는 항목은 OCTT 시험이 아니라 우리가 추가한 점검입니다 (DECISIONS D19).

## 계획했지만 아직 구현 안 됨 (지금 돌리면 실패함)
| 시험 | 필요한 기능 |
|---|---|
| TC_003_CS, TC_004_1_CS, TC_068_CS | Authorize, Start/StopTransaction |
| TC_010_CS, TC_011_1_CS, TC_012_CS | RemoteStart/RemoteStopTransaction |
| TC_013_CS, TC_014_CS | ChangeAvailability, Reset |
| TC_017_1_CS | UnlockConnector |
| OCTT 문서의 나머지 전부 | 시작 안 함 |

## 해당 없음
| 시험 | 이유 |
|---|---|
| TC_017_2_CS, TC_018_2_CS | 시뮬레이션 충전기는 케이블 분리형(고정형 아님) |
