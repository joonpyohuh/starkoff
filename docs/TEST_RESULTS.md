# 시험 결과

실제 QEMU 펌웨어를 실행한 결과만 PASS로 기록합니다. 공식 OCTT 인증 결과가 아닙니다.

## 2026-11-10 22시 · 재검증 (Claude Code) — **최신 기준**

- 아래 "3·4단계" 기록(Codex 세션)의 코드를 커밋 `4ee8375`로 묶은 뒤, 처음부터 다시 빌드(경고 0)하고
  `scripts/run_lab.sh --qemu --all --markdown`으로 68개 전체를 다시 실행했습니다.
- 결과: **67/68 PASS, 1 FAIL, 펌웨어 크래시 0건.**
- **TC_047_CS (예약 만료) FAIL**: "Step 5: 만료 뒤 8초 안에 StatusNotification(Available)을 기대했지만 아무것도
  오지 않음". 아래 기록에는 PASS로 적혀 있지만 재현되지 않았습니다. 원인은 아직 찾지 못했습니다.
  **지금은 "안 됨"으로 봅니다.**
- 재검증 뒤 추가한 기능은 **빌드만 확인했고 시험은 아직 실행하지 않았습니다** (검증 안 됨):
  StopTransaction의 transactionData, 소프트웨어 워치독(`hang` 명령), 케이블 신호 디바운싱(`noise` 명령).
  디바운싱 때문에 케이블을 꽂고 뽑은 것이 약 120 ms 뒤에 반영되므로, 기존 시험도 다시 돌려 확인해야 합니다.

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
| TC_047_CS | Reservation expiry releases connector | PASS → **재검증 FAIL** | 위 재검증 참고 (8 s) |
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
- StopTransaction.transactionData: 구현함, **검증 안 됨**. 시작 · 최근 6개 주기 샘플 · 종료 값을 1 KB 안에
  들어가는 만큼만 담습니다. RAM에만 있어서 재부팅을 넘긴 거래는 비어 있습니다.
- 출력 큐 16개(종료용 2칸 확보), 로컬 목록 32개, 한 번의 목록 전송 16개, 캐시 32개입니다.
  용량 경계의 장시간 스트레스·임의 QEMU 강제 종료·호스트 디스크 장애는 검증하지 않았습니다.
- 소프트웨어 워치독(30초)과 케이블 신호 디바운싱: 구현함, **검증 안 됨**.
- README 1절 설치 절차: **빈 HOME에서 1.2~1.6절을 검증함** (아래 "설치 절차 검증"). apt 패키지(1.1절)를
  새로 설치하는 부분과, 완전히 새 Ubuntu 22.04(새 WSL·새 PC)에서의 확인은 **아직 하지 않았습니다.**
- 공식 OCTT·스타코프 시험 서버에서는 아직 실행하지 않았습니다.
- 캐시 LRU 모든 경계, 모든 설정 조합 및 모든 메시지 스키마 오류 입력을 망라한 시험은 아닙니다.

## 설치 절차 검증 (2026-11-10)

- 명령: `scripts/check_clean_install.sh <빈 디렉터리>`. README.md의 1.2·1.3·1.4절 bash 블록을 **README에서 직접 뽑아**
  `env -i`(환경 변수 없음), 빈 `HOME`인 셸에서 순서대로 실행하고, 1.6절 명령으로 시험 하나(TC_019_CS)를 돌립니다.
- 결과: **성공 (91초, TC_019_CS PASS).** GitHub에서 받은 커밋 `e296699` 기준.
- 섞이지 않았는지 확인: SDK는 빈 HOME 안에 새로 설치된 `zephyr-sdk-0.17.2`(1.6 GB), QEMU도 그 SDK 것,
  Zephyr는 새로 받은 `413b789` = 공식 `v4.2.0` 태그 커밋, 모듈은 mbedtls · picolibc 두 개뿐.
- 한계: 1.1절 apt 패키지는 개발 PC에 이미 설치된 것을 사용했습니다(설치에 root 필요). 19개 패키지가 모두 설치돼
  있는 것만 확인했습니다.

## OCTT 충전기 시험 107개 분류 (2026-11-10)

| 분류 | 개수 | 시험 |
|---|---|---|
| 시험 있음, 실행함 | 59 | 위 표 (TC_047 재검증 FAIL 포함) |
| 시험 만듦, **아직 실행 안 함** | 12 | TC_007_1, 007_2, 061_1, 061_2, 032_1, 034, 036, 037_1, 037_2, 037_3, 038, 041 (`lab/tests/resilience.py`) |
| 해당 없음 | 6 | 아래 "해당 없음" |
| 미지원 프로파일 | 30 | FirmwareManagement: TC_044_1~3, 045_1~2 · SmartCharging: TC_056~060, 066, 067, 072, 082 · 보안 확장(TLS · 인증서 · 보안 로그 · 비밀번호): TC_073~081, 083~087 |

## 해당 없음

- TC_017_2_CS, TC_018_2_CS: 케이블 고정형 시험입니다. 이 구현은 분리형입니다.
- TC_043_1_CS, TC_048_4_CS, TC_050_4_CS: 해당 프로파일 미지원 충전기 대상입니다.
- TC_042_1_CS: 목록을 비활성화했을 때 -1 응답 경로는 구현했지만 이번 시험 묶음에서 별도 검증하지 않았습니다.
