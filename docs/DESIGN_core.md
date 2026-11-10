# 기본 기능 설계 (Core Profile 나머지 + 추가 기능)

작성: 허준표 · MVP 이후 범위. 1~9절은 Core Profile, 10~14절은 4단계(추가 기능 프로파일). 근거: OCPP 1.6 Edition 2, OCTT 테스트 케이스 문서.

## 1. 시계
- OCPP의 모든 시각은 UTC. 나라별 시간대 처리는 하지 않는다.
- BootNotification.conf와 Heartbeat.conf의 `currentTime`으로 시계를 맞춘다 (규격 4.2: 동기화 권장).
- 현재 시각 = 서버가 알려준 시각 + (지금 uptime − 그때 uptime). 하트비트마다 다시 맞춘다.
- 서버 시각을 받기 전에는 시각이 "미확정" 상태. 이때 만든 기록은 uptime만 저장해두고, 시각이 확정되면 환산한다.

## 2. 부팅 상태별 행동 (규격 4.2, TC_001, TC_002)
| BootNotification 결과 | 충전기 행동 |
|---|---|
| Accepted | 정상 동작. interval로 하트비트 |
| Pending | 먼저 요청을 보내지 않는다(일방향). 서버 요청(GetConfiguration, ChangeConfiguration 등)에는 응답한다. RemoteStart/RemoteStop은 거절. interval 후 BootNotification 재전송 |
| Rejected | interval 동안 어떤 메시지도 보내지 않고, 서버 요청에도 응답하지 않는다. interval 후 재전송 |
- Accepted 전에는 큐에 쌓인 메시지도 보내지 않는다 (규격: 부팅 완료 전 다른 요청 금지).
- interval이 0이면 자체 대기 시간(예: 30초) 사용.

## 3. 인증 후 연결 대기 타이머 (TC_004_2, TC_011_2)
- 설정키 `ConnectionTimeOut` (기본 30초). 서버가 바꾸면 그 값을 따른다.
- 카드 인증 후든 RemoteStart 후든 같은 타이머 하나를 쓴다. 시간 안에 케이블이 안 꽂히면 취소하고 Available로 복귀.

## 4. 부모 카드 (TC_069)
- 펌웨어: 트랜잭션을 시작한 idTag의 `parentIdTag`(Authorize/StartTransaction 응답의 idTagInfo)를 기억한다. 다른 카드로 종료 요청이 오면 Authorize로 그 카드의 parentIdTag를 받아 비교, 같으면 종료 허용.
- 그룹(회사) 정의는 서버 몫. lab CSMS의 테스트 카드 데이터에 회사별 그룹 코드를 둔다 (예: STARKOFF-CORP 카드 2장, OTHER-CORP 카드 1장).

## 5. 충전 중 리셋 (TC_015, TC_016)
`DESIGN_resilience.md`의 "트랜잭션 기록 + 비휘발 메시지 큐"를 그대로 쓴다.
- Soft: 트랜잭션 종료(reason=SoftReset) → StatusNotification(Finishing) → 전송 완료(또는 큐 저장) 확인 후 재부팅.
- Hard: StopTransaction(reason=HardReset)을 큐에 저장 → 즉시 재부팅 → Boot Accepted 후 큐에서 전송. (시험 문서상 재부팅 전/후 모두 허용)
- 예비 전원 없는 정전과 같은 경로를 공유한다.

## 6. 커넥터와 케이블
- 커넥터 1개, 케이블 비고정형(Not fixed cable). 케이블 고정형 시험(TC_017_2, TC_018_2)은 "해당 없음"으로 명시.
- 커넥터 0은 "충전기 전체". 상태 알림은 커넥터 0과 1 모두 보낸다. 0번으로 RemoteStart가 오면 거절.

## 7. 미터값 (TC_070, TC_071)
- Sampled: `MeterValueSampleInterval`초마다, 트랜잭션 중에만.
- Clock-aligned: `ClockAlignedDataInterval`초 간격, UTC 자정 기준 정렬. 다음 정렬 시각까지 남은 초를 계산해 타이머를 건다. 1번 시계 필요.

## 8. 거절 패턴 (쉬움, 한 곳에 모은다)
- RemoteStartTransaction 거절: connectorId가 0이거나 없는 번호 / 이미 트랜잭션 진행 중 / Faulted·Unavailable / Pending 상태
- RemoteStopTransaction 거절: 없는 transactionId
- UnlockConnector: 없는 커넥터 → NotSupported, 잠금 고장 스위치 켜짐 → UnlockFailed, 충전 중이면 먼저 종료 후 해제
- 잘못된 카드: Authorize 결과가 Accepted가 아니면 시작하지 않음
- 잠금 실패(TC_024): 케이블 꽂았는데 잠금 고장이면 충전 시작 안 함 + Faulted(ConnectorLockFailure)
- ChangeConfiguration: 모르는 키 → NotSupported, 읽기 전용 키 → Rejected, 범위 밖 값(예: 음수 간격) → Rejected. 키마다 허용 범위를 표로 정의

## 9. 데이터로 설정값 정하기 (lab 전용, 펌웨어 변경 없음)
- lab CSMS가 받은 메시지로 이벤트 시간을 SQLite(파이썬 내장)에 저장: 인증 시각, 케이블 연결(Preparing) 시각, 충전 시작·종료, 원격 시작 후 연결까지 걸린 시간.
- 웹 UI에 "카드 → 케이블 연결까지 평균·최대 시간"을 보여주고, ConnectionTimeOut 같은 설정값을 데이터 근거로 정하는 구조를 시연한다.
- 지금 쌓이는 데이터는 직접 조작한 데모 데이터라는 점을 README에 명시한다.

## 목표 시험 (1~9절)
TC_002, TC_004_2, TC_005_1, TC_005_2, TC_005_3, TC_011_2, TC_015, TC_016, TC_017_1, TC_018_1, TC_023_4, TC_023_5, TC_024, TC_026, TC_027, TC_028, TC_030, TC_031, TC_040_1, TC_040_2, TC_069, TC_070, TC_071
(TC_017_2, TC_018_2는 케이블 고정형이라 해당 없음)

---

# 4단계 · 추가 기능 프로파일

지원 프로파일: Core, LocalAuthListManagement, Reservation, RemoteTrigger (`SupportedFeatureProfiles`에 명시).
FirmwareManagement, SmartCharging은 별도 결정 전까지 미지원.

## 10. 로컬 인증 목록 (규격 3.5.2, 3.5.3, 5.x · TC_042_2, TC_043, TC_043_2, TC_043_3, TC_008_1, TC_008_2)
- 목록 내용은 서버가 정한다. 충전기는 SendLocalList로 받은 것 외에는 목록을 바꾸지 않는다 (규격 SHALL NOT).
- 버전: 목록이 없으면 0. Full은 통째로 교체, Differential은 추가 · 수정 · 삭제만 적용.
- Differential의 기준 버전이 현재 버전과 맞지 않으면 VersionMismatch. (정확한 판정 조건은 TC_043_2 시나리오를 보고 맞춘다)
- 최대 개수 `LocalAuthListMaxLength`(예: 64), 한 번에 받는 최대 `SendLocalListMaxLength`. 넘으면 Failed. 시뮬레이터의 저장소 고장 스위치로도 Failed를 재현 (TC_043_3).
- 목록은 비휘발 저장소에 보관 (재부팅 · 정전 후 유지).

## 11. 인증 순서와 캐시 정리
- 판단 순서: **로컬 목록 → 인증 캐시 → (온라인이면) 서버에 Authorize**. 로컬 목록이 캐시보다 우선 (규격 3.5.3 SHALL).
- 로컬 목록에 있는 카드는 캐시에 넣지 않는다 (규격 SHALL NOT).
- 로컬 목록에서 Accepted가 아닌 카드(Invalid, Blocked, Expired)는 오프라인이어도 거절.
- 목록과 서버 응답이 다르면 StatusNotification(connectorId=0, errorCode=LocalListConflict)로 알린다.
- 캐시가 꽉 차면: 1) 유효하지 않은 항목부터 삭제 2) 그다음 **마지막 사용 시각이 가장 오래된** 항목 삭제. 자주 쓰는 카드가 캐시에 남도록 하기 위함 (규격 3.5.1의 "oldest valid entries"를 마지막 사용 시각 기준으로 해석).

## 12. 예약 (규격 5.13, 5.3 · TC_046_1, TC_046_2, TC_047, TC_048_1~3, TC_049, TC_050_1~3, TC_051, TC_052, TC_053_1, TC_053_2)
- ReserveNow 결과: 고장이면 Faulted, 사용 중이거나 이미 예약됐으면 Occupied, 사용 불가면 Unavailable, 아니면 Accepted. 같은 reservationId가 오면 기존 예약을 교체.
- 예약된 커넥터 상태는 Reserved. 예약 카드 또는 **같은 parentIdTag(회사 그룹)** 카드만 사용 가능, 나머지는 거절. parentIdTag는 로컬 목록 · 캐시에서 찾고 없으면 Authorize로 확인 (3 · 4절 부모 카드 로직 재사용).
- 만료: 예약마다 **expiryDate에 타이머 하나**를 건다 (주기적 폴링 안 함). 시계가 재동기화되면 타이머를 다시 계산.
- 예약 종료 조건: 예약 카드로 트랜잭션 시작, 만료, CancelReservation, 커넥터가 고장 · 사용 불가로 바뀜.
- CancelReservation: 일치하는 reservationId가 없으면 Rejected.
- 커넥터 0 예약: `ReserveConnectorZeroSupported=true`. 특정 커넥터를 잡지 않고, 예약 기간 동안 예약 카드용으로 커넥터 하나가 비어 있도록 보장. 커넥터 선택은 **첫 번째 빈 커넥터**(결정적 동작, 테스트 재현성 때문에 랜덤 대신 선택). 커넥터가 1개이므로 사실상 1번이 해당. 상태 표시는 TC_049 시나리오의 기대값을 확인해서 맞춘다.
- 예약은 비휘발 저장소에 보관.

## 13. 트리거 메시지 (TC_054, TC_055)
- 지원: BootNotification, Heartbeat, StatusNotification, MeterValues(context=Trigger).
- 기능을 만들지 않은 항목(DiagnosticsStatusNotification, FirmwareStatusNotification)은 NotImplemented로 응답. 만약 시험에서 Idle 상태 응답을 요구하면 "진행 중인 작업 없음 = Idle"로 보내는 것을 검토.
- 말이 안 되는 요청(없는 커넥터 등)은 Rejected. 응답(.conf)을 먼저 보내고 그다음 해당 메시지를 보낸다.

## 14. 데이터 전송 (TC_062)
- 모르는 vendorId면 UnknownVendorId(또는 Rejected) 응답. 그 외 처리는 하지 않는다.

## 해당 없음으로 명시할 시험
TC_042_1, TC_043_1 (로컬 목록 미지원 충전기용), TC_048_4, TC_050_4 (예약 미지원 충전기용) — 해당 기능을 지원하므로 적용 대상이 아님.

## 목표 시험 (10~14절, 22개)
TC_042_2, TC_043, TC_043_2, TC_043_3, TC_008_1, TC_008_2, TC_046_1, TC_046_2, TC_047, TC_048_1, TC_048_2, TC_048_3, TC_049, TC_050_1, TC_050_2, TC_050_3, TC_051, TC_052, TC_053_1, TC_053_2, TC_054, TC_055, TC_062
