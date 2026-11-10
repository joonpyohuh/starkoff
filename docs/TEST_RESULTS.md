# 시험 결과

실제 QEMU 펌웨어를 실행한 결과만 PASS로 기록합니다. 공식 OCTT 인증 결과가 아닙니다.

## 2026-11-10 · 3·4단계와 웹 UI 구현

- 기준: `docs/reference/assignment.md`, OCPP 1.6 Edition 2 전체 추출 텍스트,
  OCTT 2026-02 전체 추출 텍스트, `docs/DESIGN_core.md`.
- 환경: Zephyr v4.2.0, `qemu_x86`, 개발 PC Ubuntu 22.04. 실제 펌웨어를 빌드하고 가짜 CSMS에 연결.
- 전체 회귀시험: **64/64 PASS**, FAIL/ERROR 0. 명령:
  `scripts/run_lab.sh --qemu --all --markdown --quiet`.
- 전체 시험 이후 Pending 상태에서 저장 큐 앞에 트리거 요청을 전달하는 처리, StartTransaction 최종
  처리 실패 시 릴레이를 여는 처리, 긴 메시지 로그를 잘라 기록하는 처리를 보완했습니다.
  이에 대한 추가 시험 결과는 아래 별도 기록합니다.
- 원본 실행 로그: `lab/results/stages34.txt` (로컬 산출물, git 제외).
- `X_` 항목은 규격·설계의 추가 점검이며 OCTT 케이스 수에 포함하지 않습니다.
- 시험 시나리오는 문서에서 구현 범위에 맞게 작성했습니다. 일부 간격·대기시간은 3초 등으로 줄였으며,
  실제 공식 OCTT의 모든 검증과 동일하다고 주장하지 않습니다.

| Test case | Name | Result | Details |
| TC_001_CS | Cold Boot Charge Point | PASS | re-boot after 10.0 s (Rejected interval 10 s); heartbeat gap 10.0 s (interval 10 s) (37 s) |
| TC_002_CS | Cold Boot Charge Point - Pending | PASS | heartbeat gap 10.0 s (interval 10 s) (39 s) |
| TC_019_CS | Retrieve configuration | PASS | 34 keys, GetConfigurationMaxKeys=50 (1 s) |
| TC_021_CS | Change/set Configuration | PASS |  (1 s) |
| TC_003_CS | Regular Charging Session - Plugin First | PASS | transactionId 1 (1 s) |
| TC_004_1_CS | Regular Charging Session - Identification First | PASS |  (8 s) |
| TC_068_CS | Stop transaction - IdTag in StopTransaction matches IdTag in StartTransaction | PASS |  (13 s) |
| TC_010_CS | Remote Start Charging Session - Cable Plugged in First | PASS |  (2 s) |
| TC_011_1_CS | Remote Start Charging Session - Remote Start First | PASS |  (5 s) |
| TC_012_CS | Remote Stop Charging Session | PASS |  (7 s) |
| TC_013_CS | Hard Reset Without transaction | PASS |  (9 s) |
| TC_014_CS | Soft Reset Without Transaction | PASS |  (6 s) |
| TC_017_1_CS | Unlock connector - no charging session running (Not fixed cable) | PASS |  (2 s) |
| X_NOTIMPL | Unknown action -> CALLERROR NotImplemented | PASS |  (1 s) |
| X_RECONNECT | Connection loss and reconnect without reboot | PASS | reconnected, first message StatusNotification(connectorId=0) (5 s) |
| X_REJECTED_SILENT | While Rejected the CP answers nothing | PASS | no answer while Rejected, BootNotification retried after the interval (26 s) |
| TC_004_2_CS | Identification first - ConnectionTimeOut | PASS |  (5 s) |
| TC_011_2_CS | Remote start - ConnectionTimeOut | PASS |  (5 s) |
| TC_005_1_CS | EV disconnect - stop=True, unlock=True | PASS |  (8 s) |
| TC_005_2_CS | EV disconnect - stop=True, unlock=False | PASS |  (4 s) |
| TC_005_3_CS | EV disconnect - stop=False, unlock=False | PASS |  (4 s) |
| TC_015_CS | Hard reset during transaction | PASS |  (7 s) |
| TC_016_CS | Soft reset during transaction | PASS |  (9 s) |
| TC_069_CS | Stop using same parentIdTag | PASS |  (4 s) |
| TC_027_CS | RemoteStartTransaction - Rejected | PASS |  (4 s) |
| TC_031_CS | UnlockConnector - NotSupported | PASS |  (1 s) |
| TC_040_1_CS | ChangeConfiguration - NotSupported | PASS |  (1 s) |
| TC_040_2_CS | ChangeConfiguration - Rejected | PASS |  (2 s) |
| TC_055_CS | TriggerMessage - Rejected | PASS |  (1 s) |
| TC_062_CS | DataTransfer - UnknownVendorId | PASS |  (1 s) |
| TC_042_2_CS | Empty local list reports version zero | PASS |  (1 s) |
| TC_043_CS | Full and differential local list updates | PASS |  (1 s) |
| TC_043_2_CS | Local list version 2 -> 5 -> reject 4 | PASS |  (1 s) |
| TC_043_3_CS | Local list storage failure preserves previous version | PASS |  (1 s) |
| TC_008_1_CS | Local list authorization - card | PASS |  (2 s) |
| TC_008_2_CS | Local list authorization - remote | PASS |  (7 s) |
| TC_046_1_CS | Reservation transaction - connector 1, remote=False, parent=False | PASS |  (6 s) |
| TC_046_2_CS | Reservation transaction - connector 1, remote=True, parent=False | PASS |  (9 s) |
| TC_049_CS | Reservation transaction - connector 0, remote=True, parent=False | PASS |  (6 s) |
| TC_053_1_CS | Reservation transaction - connector 1, remote=False, parent=True | PASS |  (7 s) |
| TC_053_2_CS | Reservation transaction - connector 1, remote=True, parent=True | PASS |  (10 s) |
| TC_047_CS | Reservation expiry releases connector | PASS |  (8 s) |
| TC_048_1_CS | Reserve connector 1 - Faulted | PASS |  (2 s) |
| TC_048_2_CS | Reserve connector 1 - Occupied | PASS |  (2 s) |
| TC_048_3_CS | Reserve connector 1 - Unavailable | PASS |  (2 s) |
| TC_050_1_CS | Reserve connector 0 - Faulted | PASS |  (4 s) |
| TC_050_2_CS | Reserve connector 0 - Occupied | PASS |  (1 s) |
| TC_050_3_CS | Reserve connector 0 - Unavailable | PASS |  (2 s) |
| TC_051_CS | Cancel reservation - Accepted | PASS |  (3 s) |
| TC_052_CS | Cancel reservation - Rejected | PASS |  (4 s) |
| TC_054_CS | Trigger MeterValues, Heartbeat, StatusNotification | PASS |  (12 s) |
| TC_070_CS | Meter values - Sample.Periodic | PASS |  (14 s) |
| TC_071_CS | Meter values - Sample.Clock | PASS |  (11 s) |
| TC_026_CS | RemoteStartTransaction - rejected while charging | PASS |  (4 s) |
| TC_028_CS | RemoteStopTransaction - rejected while charging | PASS |  (4 s) |
| TC_018_1_CS | Unlock during transaction | PASS |  (5 s) |
| TC_030_CS | Unlock motor failure | PASS |  (36 s) |
| TC_024_CS | Lock failure prevents transaction | PASS |  (3 s) |
| TC_023_4_CS | Invalid card refuses charging | PASS |  (4 s) |
| TC_023_5_CS | Invalid card refuses charging - remote | PASS |  (5 s) |
| TC_032_2_CS | Power loss queues transaction stop across reboot | PASS |  (6 s) |
| TC_039_CS | Offline start and stop replay in order | PASS |  (2 s) |
| X_LOCAL_PERSIST | Local list and reservation survive reboot | PASS |  (6 s) |
| X_LOCAL_DELETE | Differential delete and duplicate rejection | PASS |  (1 s) |

## 최종 보완 관련 시험

빌드 후 `scripts/run_lab.sh --qemu --test X_TX_RETRY --test X_PENDING_TRIGGER --test X_TX_RETRY_EXHAUSTED --test TC_019_CS --markdown --quiet`로 실행, **4/4 PASS**.

| Test case | Name | Result | Details |
| X_TX_RETRY | Transaction CALLERROR retries before advancing queue | PASS |  (7 s) |
| X_PENDING_TRIGGER | Pending trigger bypasses persisted transaction backlog | PASS |  (12 s) |
| X_TX_RETRY_EXHAUSTED | StartTransaction processing failure stops energy after final attempt | PASS |  (2 s) |
| TC_019_CS | Retrieve configuration | PASS | 34 keys, GetConfigurationMaxKeys=50 (3 s) |

전체 64개와 추가 4개를 합쳐 서로 다른 시나리오 **68개**를 실행했습니다. 전체 묶음 이후 변경은 관련 시험으로 검증했습니다.

마지막 미터 타이머 보완 후 `scripts/run_lab.sh --qemu --test X_METER_RECONFIG --test TC_070_CS --test TC_071_CS --markdown --quiet`로 실행, **3/3 PASS**.

| Test case | Name | Result | Details |
| X_METER_RECONFIG | Meter interval changes apply immediately; alignment restarts at UTC midnight | PASS |  (10 s) |
| TC_070_CS | Meter values - Sample.Periodic | PASS |  (13 s) |
| TC_071_CS | Meter values - Sample.Clock | PASS |  (8 s) |

## 웹 UI 검증

Chromium 158 (Playwright), 실제 QEMU와 웹 서버에서 **PASS**.

- 키보드로 카드 제시 → 준비 → 케이블 연결 → 충전 → 원격 종료 → 케이블 분리.
- 예약·취소, 미터값 요청, 로컬 목록 버전 조회.
- SQLite 데모 연결 시간의 표본 표시.
- 여러 줄 제어 명령 주입 거절.
- 웹에서 TC_055_CS 실행 및 PASS 표시, 새로고침 후 결과 유지.
- 1440×1000 데스크톱, 390×844 모바일 폭 확인. 가로 넘침 수정 후 통과.
- JavaScript pageerror 0건. Python compileall·JavaScript 구문 검사 통과.
- 수정한 일반 원격 요청은 순서대로 실행하며, 자동 시험 중 수동 조작은 거절합니다.

재현: `scripts/run_lab.sh --qemu --web --quiet`로 서버를 시작하고,
Playwright가 설치된 별도 Python 환경에서 `python lab/web/check_ui.py`를 실행합니다.
일반 UI 사용에는 Playwright가 필요 없습니다.
화면 산출물: `lab/results/web-desktop.png`, `lab/results/web-mobile.png` (git 제외).
헤드리스 테스트 PC에는 한글 글꼴이 없어 화면 산출물 일부 한글이 네모로 표시되지만,
문자열·상태·조작은 DOM 기반으로 검증했습니다. 실제 사용자 브라우저는 설치된 글꼴을 사용합니다.

## 미구현 또는 별도 검증이 필요한 범위

- FirmwareManagement, SmartCharging, 보안 확장(TLS·인증서·보안 로그)은 미지원입니다.
- StopTxnSampledData/StopTxnAlignedData에 따른 StopTransaction.transactionData 상세 집계는 미구현입니다.
- 출력 큐 16개(종료용 2칸 확보), 로컬 목록 32개, 한 번의 목록 전송 16개, 캐시 32개입니다.
  용량 경계의 장시간 스트레스·임의 QEMU 강제 종료·호스트 디스크 장애는 검증하지 않았습니다.
- 소프트웨어 워치독과 노이즈 디바운싱 보너스는 미구현입니다.
- 깨끗한 새 Ubuntu 22.04에서 README만으로 처음부터 설치하는 절차는 아직 검증하지 않았습니다.
- 공식 OCTT·스타코프 시험 서버에서는 아직 실행하지 않았습니다.
- 캐시 LRU 모든 경계, 모든 설정 조합 및 모든 메시지 스키마 오류 입력을 망라한 시험은 아닙니다.

## 해당 없음

- TC_017_2_CS, TC_018_2_CS: 케이블 고정형 시험입니다. 이 구현은 분리형입니다.
- TC_043_1_CS, TC_048_4_CS, TC_050_4_CS: 해당 프로파일 미지원 충전기 대상입니다.
- TC_042_1_CS: 목록을 비활성화했을 때 -1 응답 경로는 구현했지만 이번 시험 묶음에서 별도 검증하지 않았습니다.
