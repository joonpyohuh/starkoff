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
| TC_047_CS | Reservation expiry releases connector | PASS → 재검증 FAIL → **A3에서 수정 후 PASS** | 시계 오차 2~3초가 원인 (위 A3 참고) |
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
- StopTransaction.transactionData: 검증함(X_TXDATA). 종료 값 자리를 먼저 잡고 남는 공간에 시작 값과 주기 샘플을
  담습니다. 측정 항목이 많으면 종료 값만 들어갑니다. RAM에만 있어서 재부팅을 넘긴 거래는 비어 있습니다.
- 출력 큐 16개(종료용 2칸 확보), 로컬 목록 32개, 한 번의 목록 전송 16개, 캐시 32개입니다.
  용량 경계의 장시간 스트레스·임의 QEMU 강제 종료·호스트 디스크 장애는 검증하지 않았습니다.
- 소프트웨어 워치독(30초)과 케이블 신호 디바운싱: 검증함(X_WATCHDOG, X_DEBOUNCE). 디바운싱이 기존 시험에
  주는 영향은 A2 회귀에서 확인 예정.
- README 1절 설치 절차: **빈 HOME에서 1.2~1.6절을 검증함** (아래 "설치 절차 검증"). apt 패키지(1.1절)를
  새로 설치하는 부분과, 완전히 새 Ubuntu 22.04(새 WSL·새 PC)에서의 확인은 **아직 하지 않았습니다.**
- 공식 OCTT·스타코프 시험 서버에서는 아직 실행하지 않았습니다.
- 캐시 LRU 모든 경계, 모든 설정 조합 및 모든 메시지 스키마 오류 입력을 망라한 시험은 아닙니다.

## 최종 · 심사위원 기준 전체 실행 (2026-11-11, 커밋 `c275be5`) — **최신 기준**

- 방법: `scripts/check_clean_install.sh --all <빈 디렉터리>` (빈 HOME에서 GitHub clone → 설치 → 빌드 → 83개 전부).
- 설치: 성공(685초, 시험 포함). Zephyr `413b789`(v4.2.0), SDK는 빈 HOME에 새로 설치된 0.17.2.
- 결과: **82 PASS, 0 FAIL, 1 ERROR, 펌웨어 크래시 0건.**
- ERROR 1건(TC_015_CS)은 **시험 도구 문제**였습니다. 시험 시작 전 정리 단계에서 서버 요청을 5초만 기다렸는데,
  충전기 응답이 네트워크에서 5초 넘게 늦게 도착했습니다. 펌웨어 로그상 충전기는 요청을 **받은 즉시** 답했습니다.
  늦어진 이유는 Zephyr e1000 드라이버가 수신 버퍼 1개뿐이라, 메시지 두 개가 동시에 오면 하나를 놓치고 TCP가
  1 → 2 → 4초 뒤 다시 보내기 때문입니다(README 5절). → 정리 단계 대기를 15초로 늘리고, 실패하면 ERROR 대신
  이유가 보이는 "준비 단계 실패"로 나오게 고쳤습니다. 같은 순서(TC_005_3 → TC_015)로 2번 다시 돌려 모두 PASS.
- ERROR가 나면 이제 오류 위치(파일:줄)도 함께 기록합니다.
- 고친 것은 시험 도구(`lab/`)뿐이라 펌웨어 결과는 바뀌지 않습니다. 고친 뒤 83개 전체 재실행은 아직 하지 않았습니다.

## 심사위원 기준 전체 실행 (2026-11-10 23시, 커밋 `e36f632`)

- 방법: `scripts/check_clean_install.sh --all <빈 디렉터리>`. 빈 HOME · 빈 환경에서 README 1.2~1.4절대로
  GitHub에서 clone → Zephyr v4.2.0 · SDK 0.17.2 새로 설치 → 빌드 → 실험실 설치 후 83개 전부 실행.
- 설치: 성공(654초, 시험 포함). 시험: **80/83 PASS, 3 FAIL, 펌웨어 크래시 0건.**
- 실패 3건은 모두 **시험 코드 문제**로 확인하고 고쳤습니다(고친 뒤 해당 시험 재실행 PASS):
  - X_TXDATA: 케이블을 뽑자마자 다음 RemoteStart를 보냄. 디바운싱(~120 ms) 동안 커넥터가 아직 Finishing이라
    펌웨어가 규칙대로 Rejected. → Available을 기다린 뒤 보냄.
  - TC_037_2, TC_037_3: 재연결 직후 오는 커넥터 0의 Available을 커넥터 1의 상태로 잘못 읽음. → 커넥터 1만 봄.
- 이 실행에서 TC_047은 통과했지만 이전 실행에서는 실패 → 아래 A3에서 원인을 찾아 고침.

## A3 · TC_047 원인과 수정 (2026-11-11)

- 원인: **충전기 시계가 실제보다 2~3초 늦었습니다.** 서버 시각의 소수점 아래와 흐른 시간을 각각 초 단위로
  내림해서 생긴 오차입니다. 그래서 3초 뒤 만료될 예약이 5~6초 뒤에 풀렸고, 시험의 8초 대기 안에 들어갈지가
  상황에 따라 달랐습니다. 이 시계는 모든 timestamp, 예약 만료, 로컬 목록 만료에 쓰이므로 펌웨어 버그입니다.
- 수정: 시계를 밀리초 단위로 유지(`firmware/src/ocpp/clock.c`). 수정 뒤 예약 만료 3.0초, 충전기와 호스트의 시각
  차이는 1초 안(충전기는 초 단위까지만 표시).
- 같이 찾은 시험 문제: X_METER_RECONFIG가 충전기 시계를 가짜 시각(23:59:58)으로 둔 채 끝나서, 바로 뒤에 도는
  TC_036이 실패했습니다(시험 순서에 따라 결과가 달라짐). → 모든 시험 시작 전에 TriggerMessage(Heartbeat)로
  충전기 시계를 진짜 시각으로 다시 맞춥니다. 그 순서로 다시 돌려 TC_036 · TC_047 PASS.
- 수정 후 심사위원 기준 전체 실행: 위 "최종" 항목 참고(82/83, 남은 1건은 시험 도구 문제로 수정).

## A1 · 새 기능 검증 (2026-11-11)

| 점검 | 결과 | 측정값 |
|---|---|---|
| X_WATCHDOG (`hang`) | PASS | 상태등 2.0초 뒤 꺼짐, 32.2초 뒤 재부팅 + BootNotification, 로그에 `watchdog: main loop stuck for 30 s` |
| X_DEBOUNCE (`noise`) | PASS | 노이즈 15초 동안 순간 신호 184~186개 걸러냄, 상태 변화 0회. 노이즈 없을 때 인식 지연 171~238 ms(상태 조회 간격 포함) |
| X_TXDATA | PASS | 측정 항목 1개: Begin + 주기 3개 + End. 4개: End만 들어감(570바이트, 1 KB 한도) |

검증하다가 고친 것: 워치독 재부팅 메시지가 지연 로그에 묻혀 사라짐(`LOG_PANIC` 추가), 1 KB가 차면
Transaction.End가 빠짐(End 자리를 먼저 확보), 측정 항목 4개짜리 샘플이 임시 버퍼(512 B)를 넘어 하나도 안 들어감
(1 KB로 늘림, 기본값인 `location` 생략). 시험 자체의 버그 1건(직전 실제 변화의 알림을 노이즈로 오판)도 고침.

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
