# 오프라인 · 정전 · 고장 대응 설계

작성: 허준표 · 기준 문서: OCPP 1.6 Edition 2 (3.7, 4.6, 5.x, 9.1), OCTT 테스트 케이스 2.4 / 2.14 / 2.15 / 2.17

## 설계 원칙: "3단 방어"

충전기는 서버와 연결이 끊겨도, 전원이 나가도, 스스로 판단하고 기록했다가 결국 알려야 한다.

| 단계 | 원칙 | 이 과제에서의 구현 |
|---|---|---|
| 1단 · 기기 혼자서 | 감지 → 기록 → 안전 → 복구 | 연결 끊김 감지, 트랜잭션 메시지 큐 영속화, 로컬 인증, 재연결 후 순서대로 전송 |
| 2단 · 서버가 보는 신호 | "조용함 자체가 신호" | Heartbeat 주기 전송, 응답 타임아웃으로 오프라인 판정 |
| 3단 · 고장 구분 | 정전과 CPU 고장을 헷갈리지 않게 | 정전은 명시적 이벤트로 처리, CPU 정지는 "정상일 때만 초록불"로 드러냄 |

(블루투스 릴레이, 게이트웨이 승계, AI 진단은 OCPP 1.6 시험 범위 밖이라 이 과제에서는 구현하지 않는다. 문서로만 남긴다.)

---

## 1단 · 기기 혼자서

### 1-1. 감지: 언제 "오프라인"으로 볼 것인가
- WebSocket이 닫히거나 연결 실패 → 즉시 오프라인
- 보낸 CALL에 응답이 `CallTimeout`(기본 30초) 안에 없으면 → 오프라인으로 보고 재연결 시도
- 재연결은 간격을 늘려가며 시도 (예: 5 → 10 → 20 → 최대 60초). 서버를 두드리는 폭주 방지

### 1-2. 기록: 트랜잭션 메시지 큐 (규격 3.7)
- **트랜잭션 관련 메시지**(StartTransaction, StopTransaction, 트랜잭션 중 MeterValues)는 오프라인이면 **큐에 쌓는다** (규격: MUST queue)
- 연결이 돌아오면 **시간 순서대로** 보낸다. 큐가 빌 때까지 **새 트랜잭션 메시지는 뒤에 줄 선다** (규격: SHALL wait until the queue has been emptied)
- 큐는 **비휘발 저장소에 보관**한다. 정전이 나도 기록이 사라지지 않게
- 큐 크기는 고정(예: 64개). 꽉 차면 가장 오래된 MeterValues부터 버리고, Start/Stop은 절대 버리지 않는다. 버린 개수는 로그로 남긴다
- 서버가 CALLERROR로 답하면 `TransactionMessageAttempts`번까지, `TransactionMessageRetryInterval × 시도 횟수`초 기다렸다가 재전송 (규격 3.7.1). 그래도 실패하면 버리고 다음으로 넘어간다 (큐가 영원히 막히지 않게)

**오프라인 중 시작한 트랜잭션의 ID 문제**
- 서버가 transactionId를 주기 전이므로, 내부적으로 임시 ID(음수 등)를 붙여 큐에 넣는다
- StartTransaction.conf로 진짜 ID를 받으면, 큐에 남은 같은 트랜잭션의 MeterValues / StopTransaction의 ID를 진짜 ID로 바꿔서 보낸다

**미터값 (TC_036)**
- 오프라인이어도 `MeterValueSampleInterval`마다 샘플을 만들어 타임스탬프와 함께 큐에 넣는다
- 재연결 후 **오래된 것부터** 보낸다. 형식은 Raw, context는 Sample.Periodic

### 1-3. 안전: 서버 없이 내리는 판단
**로컬 인증 (규격 5.x, 9.1)**
- `LocalAuthorizeOffline=true`이면 오프라인일 때 인증 캐시(+ 이후 Local Auth List)에서 idTag를 찾아 Accepted면 충전 시작
- 캐시에 없으면 `AllowOfflineTxForUnknownId=true`일 때만 일단 시작
- 인증 캐시는 Authorize.conf / StartTransaction.conf의 idTagInfo로 갱신. `ClearCache`로 비움 (TC_007, TC_061)

**재연결 후 서버가 "그 카드 무효"라고 할 때 (TC_037_2, TC_037_3)**
- StartTransaction.conf의 idTagInfo가 Invalid이면:
  - `StopTransactionOnInvalidId=true` → StopTransaction(reason=DeAuthorized) → Finishing
  - `false` → 트랜잭션은 유지하되 에너지 공급만 끊음 → SuspendedEVSE

**고장 상태에서 충전 거부 (TC_041)**
- Faulted 상태에서는 새 트랜잭션을 시작하지 않는다 (로컬 카드, RemoteStart 모두)

### 1-4. 복구
- 재연결 성공 → BootNotification이 이미 Accepted 상태였다면 다시 보내지 않고, 커넥터 상태를 StatusNotification으로 다시 알린 뒤 큐 비우기 시작
- 상태 알림(StatusNotification)은 트랜잭션 메시지가 아니므로 쌓지 않고, 재연결 시 **현재 상태만** 보낸다

---

## 2단 · 서버가 보는 신호: "조용함 자체가 신호"
- BootNotification.conf의 interval(또는 `HeartbeatInterval` 설정값)마다 Heartbeat 전송 (TC_001)
- 서버 입장에서는 Heartbeat가 끊기는 것 자체가 "이 충전기에 문제가 있다"는 신호가 된다
- 충전기 입장에서는 Heartbeat 응답이 안 오는 것도 오프라인 판정 근거로 쓴다 (1-1과 같은 타임아웃)

---

## 3단 · 고장 구분: 정전 vs CPU 정지

### 3-1. 정전 (TC_032_1, TC_032_2, TC_034)
정전은 시뮬레이터의 `power off` / `power on` 이벤트로 일으킨다.

| 설정 `BackupPower` (자체 설정키) | 정전 순간 | 전원 복구 후 |
|---|---|---|
| true (예비 전원 있음) | StopTransaction(reason=PowerLoss) → StatusNotification(Finishing) 보내고 꺼짐 | Boot → 상태 알림 (Finishing 또는 Preparing) |
| false (예비 전원 없음, 기본값) | 아무것도 못 보내고 꺼짐 | Boot → 저장된 트랜잭션 기록 확인 → 이어서 충전하거나 PowerLoss로 종료 |

- **가용성 상태(Available / Unavailable)도 비휘발 저장소에 저장**해서, 전원이 돌아와도 Unavailable이면 Unavailable로 부팅한다 (TC_034)
- 이를 위해 "진행 중 트랜잭션 기록"을 트랜잭션 시작/종료 때마다 저장소에 쓴다

### 3-2. CPU 정지: "정상일 때만 초록불"
- 펌웨어 메인 루프가 주기적으로 `alive` 신호를 갱신한다
- 충전기 상태등(시뮬레이션)은 **최근 2초 안에 alive 신호가 있을 때만 켜진다.** 고장 시 빨간불을 켜는 방식이 아니라, 멈추면 저절로 꺼지는 방식
- 웹 UI는 제어 포트로 상태를 주기적으로 묻고, 응답이 끊기면 상태등을 끈다. 전원 표시와 상태등을 따로 보여줘서 **"전원은 켜졌는데 상태등만 꺼짐 = CPU 문제"**로 구분된다
- 추가로 소프트웨어 워치독: 메인 루프가 일정 시간 멈추면 스스로 재부팅

### 3-3. 고장 보고 (TC_041)
- 시뮬레이터로 고장을 주입한다 (예: 과전류, 과열, 잠금 실패)
- 범위 검사(정해진 바운더리를 벗어나면 고장으로 판단): 전압, 전류, 온도 값이 정상 범위를 벗어나면 StatusNotification(Faulted, errorCode=OverCurrentFailure / HighTemperature 등)
- 고장이 풀리면 Available로 복귀

---

## 보너스 · 노이즈 디바운싱 (1차 면접 질문 구현)
- 시뮬레이터의 케이블 연결 신호를 10ms마다 샘플링하는 구조로 만들고, 업다운 카운터 + 히스테리시스로 판정한다
- 제어 명령 `noise <c> on|off`로 신호에 순간 튀는 값을 섞을 수 있고, 디바운싱 덕분에 커넥터 상태가 흔들리지 않는 것을 웹 UI에서 보여준다
- OCPP 시험 항목은 아니므로 시간이 남을 때 구현. 구현하면 README에 별도 항목으로 표기

---

## 설정키 (이 설계에서 쓰는 것)

| 키 | 기본값 | 쓰임 |
|---|---|---|
| HeartbeatInterval | 서버 지정 | 2단 |
| MeterValueSampleInterval | 60 | 오프라인 미터값 큐 |
| LocalAuthorizeOffline | true | 오프라인 인증 |
| AuthorizationCacheEnabled | true | 인증 캐시 |
| AllowOfflineTxForUnknownId | false | 캐시에 없는 카드 오프라인 허용 여부 |
| StopTransactionOnInvalidId | true | 재연결 후 무효 카드 처리 |
| TransactionMessageAttempts | 3 | CALLERROR 재전송 횟수 |
| TransactionMessageRetryInterval | 60 | 재전송 간격 |
| StopTransactionOnEVSideDisconnect | true | 차 쪽 분리 시 종료 |
| BackupPower (자체 키) | false | 정전 처리 방식 선택 |

---

## 펌웨어 모듈 배치 (CLAUDE.md 폴더 구조 기준)

| 기능 | 위치 |
|---|---|
| 오프라인 판정, 재연결, 트랜잭션 메시지 큐 | `firmware/src/net/` |
| 로컬 인증, 인증 캐시, 무효 카드 처리, 설정키 | `firmware/src/ocpp/` |
| 트랜잭션 기록 · 가용성 · 큐 영속화 | `firmware/src/ocpp/` + 저장소 계층 |
| 정전 · 고장 주입, 범위 검사, 디바운싱, 상태등 | `firmware/src/sim/` |
| `power`, `fault`, `net_down/net_up`, `noise` 제어 명령 | `firmware/src/ctrl/` |

연결 끊김은 테스트에서 일으켜야 하므로 제어 명령 `net_down` / `net_up`을 추가한다 (펌웨어가 스스로 소켓을 끊고 재연결을 막는 방식).

---

## 아직 정해지지 않은 것 (구현 전에 확인)
1. **QEMU에서 비휘발 저장소를 어떻게 흉내 낼지**
   - A안: QEMU에 파일로 연결된 가상 저장 장치를 붙여서, QEMU를 껐다 켜도 남게 한다 (가장 실제에 가까움)
   - B안: 재부팅해도 지워지지 않는 RAM 영역을 쓴다 (`power off`를 펌웨어 재부팅으로 흉내 낼 때만 유효)
   - 먼저 A안을 시도하고, 안 되면 B안으로 가고 그 한계를 README에 적는다
2. 정전 시험(TC_032_1 / 032_2)은 `BackupPower` 값에 따라 둘 중 하나만 통과할 수 있다. 기본값을 무엇으로 할지, 둘 다 시연할지 결정
3. 스타코프 시험 도구가 연결 끊김과 정전을 어떻게 일으키는지 (문의 메일 답변 대기)

## 이 설계로 노리는 테스트 케이스
TC_001, TC_007_1, TC_007_2, TC_061_1, TC_061_2, TC_032_1 또는 TC_032_2, TC_034, TC_036, TC_037_1, TC_037_2, TC_037_3, TC_038, TC_039, TC_041
