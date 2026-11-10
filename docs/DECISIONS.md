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

## D7. Static IP 10.0.2.15 + "link announce" (not DHCP)
- **Finding**: Zephyr v4.2.0's e1000 driver enables receive with `RCTL_EN | RCTL_MPE` but without
  `RCTL_BAM` (accept broadcast), so QEMU drops every broadcast frame to the guest. DHCP never completes,
  and SLIRP's ARP requests for 10.0.2.15 never arrive, so the host->guest port forward (control port)
  silently hangs until the guest happens to send something first.
- **Decision**: static IP (SLIRP always uses 10.0.2.15 anyway) and `src/net/link.c` sends one tiny UDP
  datagram to the gateway at boot and every 30 s. Our own ARP request teaches SLIRP our MAC; its reply is
  unicast, so it arrives. Zephyr itself is not patched (the grader gets plain upstream v4.2.0).
- **How it was found**: control port timed out; gdb/`net conn` showed no SYN ever reached the TCP stack;
  after a manual `net ping` it worked; DHCP also timed out; then read the driver's RCTL bits.

## D8. Default server URL / CP ID are Kconfig options, set by `scripts/build.sh --url --id`
- **Why**: A build-time default is the one path that works on any machine, with no storage involved.
  Runtime change via the shell is added on top (see later decisions).

## D9. Persistent storage: ivshmem on qemu_x86 (NVMe and qemu_x86_64 rejected)
- **Problem**: `qemu_x86`'s simulated flash is RAM and is erased on every boot; `__noinit` RAM is also
  cleared on reboot (tested: boot counter always 1).
- **Tried**: NVMe disk. Does not build on 32-bit x86 (MSI helpers exist only for 64-bit). On `qemu_x86_64`
  NVMe persisted across reboot and QEMU restart (tested), and the owner chose to switch boards for it,
  but e1000 networking on x86_64 then did not work in our tests, so that path was dropped.
- **Decision**: stay on `qemu_x86` and use QEMU `ivshmem-plain`: a 1 MiB host file mapped as a PCI
  memory BAR. The firmware treats it like memory-mapped NOR flash. Tested: survives in-QEMU reboot and
  QEMU restart. On a real charger this layer would be flash/FRAM; only `src/store/store.c` would change.
- **Power-loss safety**: two slots (A/B) per record, sequence number + CRC32, header written last.
  A write cut in the middle leaves the older slot valid.

## D10. A new build's default URL / CP id win over stored ones
- **Why**: otherwise a grader who rebuilds with `--url/--id` but keeps an old `state/cp_nvm.img` would
  silently connect to the old server. The build stores a tag `url|id`; if it differs at boot, the stored
  URL/id are dropped. Other keys (HeartbeatInterval etc.) are kept. Tested.

## D11. One command parser for both the UART shell and the TCP control port
- **Why**: the web UI/tests (TCP) and a human (UART: `cp plug 1`) must get exactly the same behaviour.
  `ctrl_exec()` is shared; the transports only move lines.

## D12. The hardware simulator keeps running while "power is off"
- **Why**: the sim plays the outside world (mains, cable, car). Someone must be able to switch power back
  on. Only the charger logic stops. Documented as a simulation limit.

## D13. Readonly keys are readonly for everyone
- Even the local shell cannot change `GetConfigurationMaxKeys`, `NumberOfConnectors`,
  `SupportedFeatureProfiles`; their values always come from the build.

## D14. Control port clients must not half-close; `scripts/ctl.sh` provided
- **Finding**: `printf 'status\n' | nc -q 1 ...` got no reply. Debug log: `recv` got the data, then
  `send` failed with `ENOTCONN` (errno 128). The client's FIN arrived with the data, and Zephyr's TCP
  does not support half-close, so the connection is gone before we can answer.
- **Decision**: not worked around in firmware (it is a Zephyr TCP property). `scripts/ctl.sh` sends
  commands and waits for each reply; interactive `nc` works. The lab tools keep one connection open.
  OCPP is not affected (the firmware is the WebSocket client and the connection stays open).

## D15. JSON: jsmn (vendored) instead of Zephyr's JSON library
- **Why**: Zephyr's `json.h` needs a fixed descriptor per message shape; OCPP messages vary a lot
  (optional fields, arrays of keys). jsmn only tokenizes, allocates nothing, is one header (MIT), and
  is easy to explain. Writing JSON is a small bounds-checked append helper (`ocpp/json.c`); any overflow
  becomes a clear CALLERROR / log line instead of a cut message.

## D16. One CALL in flight, FIFO queue, entry removed only on CALLRESULT
- **Why**: OCPP-J 4.1.1 says not to send a new CALL before the previous one is answered or timed out.
  Keeping the entry until it is answered means a message lost with a dropped connection is sent again
  after reconnect. Non-transaction messages (Heartbeat, StatusNotification) are dropped on disconnect;
  the current status is reported fresh after reconnect instead (DESIGN_resilience 1-4).

## D17. Boot state rules
- Before any answer to BootNotification, and while Rejected: no requests, and CALLs from the server are
  not answered (spec 4.2 for Rejected; before registration we treat it the same).
- Pending: answer the server's CALLs, send no requests of our own, retry Boot after `interval`.
- `interval` 0 → we wait 30 s. Reconnect without reboot → no new BootNotification.

## D18. Offline detection and back-off
- Offline = socket closed/failed, or no answer to a CALL within 30 s (then we close and reconnect).
- Reconnect after 5, 10, 20, 40, then every 60 s (DESIGN_resilience 1-1). Reset to 5 s on success or
  on `net_up`. Trade-off: a CSMS started late is found within at most 60 s.

## D19. Lab checks with ids X_... are separate from OCTT test cases
- They check spec / design rules that no OCTT case in our list covers (NotImplemented, reconnect,
  silence while Rejected). Kept apart so the OCTT results table is not inflated.

## D20. 스택 넘침 버그 2건과 스택 보호 상시 켜기 (이후 기록은 한국어)
- **현상**: 첫 서버 접속 시도를 부팅 2초 뒤로 옮기자 `zsock_socket()` 안의 `errno = 0`에서 page fault.
  `errno`가 있는 스레드 로컬 영역 포인터가 망가진 상태였음.
- **원인 1**: `outq_drop_non_tx()`가 큐 전체 복사본(약 17 KB)을 8 KB 스택에 잡음. 연결이 끊길 때마다
  실행되므로, 이전에 "통과"한 실행에서도 스택 아래 메모리를 조용히 덮어쓰고 있었음.
- **원인 2**: `kernel thread stacks`로 재 보니 시스템 워크큐가 1052바이트를 사용. 원래 크기 1024바이트라
  여기도 넘치고 있었음 (`link.c`의 소켓 호출과 미터 계산이 워크큐에서 돎).
- **찾은 방법**: `CONFIG_HW_STACK_PROTECTION`을 켠 빌드로 재현 → "Stack overflow" + 호출 위치
  (`outq_drop_non_tx`). 고친 뒤 스택 사용량을 스레드별로 측정.
- **결정**: 큐는 제자리에서 압축(임시 복사본 없음), 큰 버퍼는 static, 워크큐 2 KB · 쉘 4 KB.
  **스택 보호는 제출 빌드에서도 켜 둔다.** 넘치면 조용히 메모리를 망가뜨리는 대신 바로 멈추고 위치를 알려줌.

## D21. 충전 세션 상태 머신과 transactionId 자리표시자(%TX%)
- 커넥터마다 IDLE → AUTHORIZED → CHARGING → FINISHING. OCPP 상태(7.7)는 이 단계와 하드웨어 상태로 계산
  (Faulted > Charging/SuspendedEV/SuspendedEVSE > Finishing > Preparing > Unavailable > Available).
- StartTransaction에는 내부 번호(tx_local)를 붙이고, 진짜 transactionId가 필요한 MeterValues · StopTransaction은
  `%TX%`를 넣어 큐에 넣는다. 보낼 때 실제 번호로 바꾼다. 큐는 순서대로 하나씩 나가므로 그때는 이미
  StartTransaction.conf가 와 있다. 오프라인 트랜잭션(6단계)도 같은 방식으로 처리할 수 있다.
- **왜**: StartTransaction 응답을 기다리느라 충전을 멈추지 않아도 되고, 응답이 늦어도 메시지 순서가 지켜진다.

## D22. 가용성(ChangeAvailability)은 숨은 설정 키 `_Inoperative`에 저장
- TC_013은 Hard Reset 뒤에도 커넥터가 Unavailable로 부팅되는지 본다. 이미 비휘발로 저장되는 설정 표를
  재사용해서 저장 코드를 따로 만들지 않았다. 충전 중 Inoperative 요청은 Scheduled로 답하고 끝나면 적용.

## D23. Reset은 "트랜잭션 종료 → 큐 전송 → 재부팅"
- Reset.conf(Accepted)를 먼저 보내고, 진행 중 트랜잭션을 reason=HardReset/SoftReset으로 끝낸 뒤 큐가 비면
  재부팅한다. 큐가 안 비어도 Hard는 5초, Soft는 30초 뒤에는 재부팅한다. (큐 비휘발 저장은 6단계)

## D24. RemoteStart 거절 조건 (DESIGN_core 8절)
- Pending 상태, connectorId 0, 없는 커넥터 번호, 이미 사용 중 · 인증 대기 중, Faulted, Unavailable이면 Rejected.
  connectorId가 없으면 1번을 고른다. 처음 구현에서 없는 번호(예: 5)를 1번으로 바꿔 Accepted하던 버그를
  코드 검토 중 발견해 고쳤다.

## D25. 시험 도구 버그: CSMS 요청 직후 온 메시지를 놓치던 문제
- TC_012가 실패했는데 펌웨어는 정상이었다. 시험 도구가 RemoteStop 응답을 받은 뒤 "본 위치"를 이벤트 목록
  끝으로 옮겨서, 응답 직후 도착한 StopTransaction을 건너뛰었다. 응답 이벤트 바로 다음으로 옮기도록 고쳤다.
- 같이 고친 것: 가짜 CSMS가 시험마다 transactionId를 1부터 다시 줘서 로그가 헷갈렸다. 이제 계속 증가.

## D26. 로컬 목록과 인증 캐시는 별도 저장
- `docs/DESIGN_core.md` 10~11절과 규격 3.5에 따라 로컬 목록을 먼저 조회한다. 목록 32개,
  한 번의 SendLocalList는 16개까지 받는다. 캐시는 32개이며 무효 항목, 마지막 사용이 오래된 항목 순서로 교체한다.
- Full/Differential은 임시 복사본에서 검증하고 A/B 저장에 성공한 뒤 적용한다. 중복 idTag, 잘못된 상태·시각,
  용량 초과는 Failed로 처리한다. Differential 버전은 현재보다 **크면** 허용한다(규격 7.47, TC_043_2).
- 목록이 비면 GetLocalListVersion은 0이다. 목록이 비어도 마지막 업데이트 버전은 내부에 유지한다.
  목록 저장 실패는 제어 포트 `cfg set _StoreFail true`로 주입한다. 시험 후 false로 돌린다.

## D27. 예약 만료는 타이머 하나, 그룹은 parentIdTag 비교
- 1개 커넥터 구성이라 connectorId=0 예약도 1번을 사용자에게 비워 둔다. 상태 알림은 커넥터 1에 Reserved,
  커넥터 0에는 Available/Unavailable/Faulted만 사용한다. 규격 3.8·4.9와 TC_049 기준이다.
- 예약을 비휘발 저장하고, 서버 시각 동기화 후 expiryDate까지의 시간을 계산해 Zephyr 타이머를 건다.
  타이머 콜백은 플래그만 세우고 실제 저장·상태 변경은 메인 루프가 처리한다.
- 예약 카드 또는 같은 parentIdTag 카드가 시작하면 StartTransaction에 reservationId를 넣고 예약을 해제한다.
  충전 중 다른 카드의 종료 권한도 parentIdTag로 확인한다. 부모 그룹이 없는 거래는 같은 카드로만 종료한다.

## D28. 큐와 거래 번호·하드웨어 상태를 하나의 체크포인트로 기록
- 큐만 저장하면 재부팅 후 `%TX%`를 실제 transactionId로 바꿀 수 없다. 따라서 큐와 거래 번호 매핑,
  커넥터 상태 및 가짜 하드웨어를 한 A/B 레코드에 저장한다. 큰 복사본은 static 메모리에 둔다.
- 기존 설정 레코드 위치는 유지한다. 큐 전용 슬롯은 NVM의 0x10000/0x18000에 각각 32 KiB를 배정한다.
  로컬 목록·캐시·예약은 기존 4 KiB 레코드 3·4·5를 사용한다.
- 메인 루프가 바뀐 체크포인트만 기록한다. `power off` 이벤트와 Reset 재부팅 전에는 기록을 마친다.
  QEMU 프로세스를 임의로 강제 종료하는 순간의 완전한 무손실 및 호스트 디스크 fsync는 보장하지 않는다.
- 재부팅 후 남은 진행 거래는 Boot Accepted 후 PowerLoss로 종료한다. Reset으로 이미 종료한 거래는 다시 종료하지 않는다.
- 큐는 16개 고정이며 종료 메시지용 2칸을 남긴다. 제한을 넘는 새 요청은 로그에 남기고 거절한다.
  새 StartTransaction을 기록할 수 없으면 릴레이를 열고 시작을 취소한다. 장기 오프라인에서 미터값 무손실은 보장하지 않는다.

## D29. 웹 UI는 로컬 실험실 도구
- 기존 `lab/web/` 초안을 이어서 HTML·JS·CSS와 Python websockets 서버로 구현했다. npm이나 웹 빌드는 없다.
- 카드·케이블 조작은 TCP 제어 포트, 원격 시작·예약은 가짜 CSMS의 OCPP 호출을 사용한다.
  시험 버튼은 CLI와 동일한 `lab/tests/` 시나리오를 실행한다. 시험 시 상태를 초기화하므로 데모 세션과 섞지 않는다.
- 로컬 호스트로만 바인딩하고 WebSocket Origin을 검사한다. 브라우저에서 임의 제어 줄을 여러 개 주입하지 못하도록 줄바꿈을 거절한다.
- SQLite에 OCPP 이벤트와 UI에서 카드→케이블 연결까지의 시간을 저장한다. 직접 만든 데모 데이터이며 현장 운영 통계가 아니다.
  파일: `lab/results/telemetry.sqlite3`. 평균·최대는 이 UI 조작 표본만 집계한다.

## D30. 미터 타이머는 설정 변경과 UTC 자정에서 다시 계산
- 충전 도중 MeterValueSampleInterval을 바꾸면 그 순간부터 새 간격을 적용한다. 0이면 중지한다.
- ClockAlignedDataInterval이 86400을 나누어떨어지지 않아도 다음 날 UTC 00:00부터 정렬을 다시 시작한다.
- X_METER_RECONFIG는 충전 중 간격 변경과 23:59:58→다음 날 00:00:00 경계를 가짜 서버 시각으로 검사한다.

## D31. Codex 작업은 그대로 커밋하고 다시 검증
- 다른 AI 도구(Codex) 세션의 커밋되지 않은 변경 18개 파일을 한 커밋(`4ee8375`)으로 묶고, 커밋 메시지에 출처를 적었다.
  그 기록의 "전부 통과"를 그대로 쓰지 않고 다시 실행했다: 67/68, TC_047 실패. 실패는 숨기지 않고 문서에 남긴다.

## D32. StopTransaction transactionData는 고정 크기 링 + "들어가는 것만"
- 규격 3.16.1/3.16.2: StopTxnSampledData / StopTxnAlignedData에 있는 측정 항목을 거래 중 샘플로 담는다.
- 커넥터마다 Transaction.Begin 1개 + 최근 6개 샘플(Sample.Periodic / Sample.Clock) 링 + 종료 시 Transaction.End.
  메시지 한도 1 KB 안에서 샘플을 하나씩 임시 버퍼에 써 보고 들어갈 때만 붙인다. 오래된 샘플부터 밀려난다.
- RAM에만 둔다. 비휘발 체크포인트(D28) 형식을 바꾸지 않기 위해서다. 대가: 재부팅을 넘긴 거래는 비어 있다.

## D33. 소프트웨어 워치독 30초 (task_wdt, 하드웨어 없음)
- 메인 루프가 한 바퀴마다 먹이를 준다. 30초 동안 없으면 스스로 재부팅한다. 정상적으로 가장 오래 막히는 곳이
  서버 접속 시도(TCP 10초 + 핸드셰이크 5초)라서 그 두 배로 잡았다.
- 만료 콜백은 타이머 문맥이라 지연 로그 대신 printk로 남기고 재부팅한다.
- 시연용 `hang` 명령: 메인 루프가 멈추면 alive 신호가 끊겨 상태등이 2초 안에 꺼지고(D 설계 3-2), 30초 뒤 재부팅.

## D34. 케이블 감지 디바운싱 16/12/4
- 10 ms마다 신호를 읽어 업다운 카운터(0~16)에 반영. 12 이상이면 꽂힘, 4 이하면 빠짐, 사이는 이전 상태 유지.
- 처음 생각한 8/6/2는 노이즈 25%에서 가짜 판정 6번 연속(샘플당 (1/4)^6)이 100 Hz로 약 40초에 한 번 나와
  가짜 분리가 생긴다. 16/12/4는 (1/4)^12로 약 5일에 한 번. 대가는 약 120 ms의 인식 지연.

## D35. 설치 절차 검증은 "README에서 직접 뽑아 빈 HOME에서 실행"
- Docker가 없고 root 권한도 없어서 진짜 새 PC 대신, `env -i` + 빈 `HOME`으로 이 PC의 설정 · 경로 · SDK가
  섞이지 않게 했다. 명령은 README에서 정규식으로 뽑으므로 README가 틀리면 검증도 실패한다.
- 섞이지 않았는지는 결과로 확인: CMakeCache의 SDK 경로, QEMU 경로, Zephyr 커밋(`413b789` = v4.2.0).
- 남은 빈틈: apt 패키지(1.1절). 새 WSL 배포판(`wsl --install -d Ubuntu-22.04`)에서 한 번 더 하면 메울 수 있다.

## D36. 시험이 바꾼 설정은 시험이 끝나면 되돌린다
- 새 시험(`lab/tests/resilience.py`)은 `settings()`로 원래 값을 읽어 두고, 실패해도 끝나면 되돌린다.
  예: TC_037_2가 `StopTransactionOnInvalidId=false`로 둔 채 끝나면 뒤 시험의 결과가 달라진다.
- 기존 시험(Codex 작성 포함)은 아직 이 방식이 아니다. 회귀 시험 때 순서에 따라 결과가 바뀌는지 볼 것.

## D37. A1 검증에서 배운 것
- **빌드가 실패하면 시험을 돌리지 않는다.** 한 번은 빌드 실패를 못 보고 이전 펌웨어로 시험을 돌렸다.
  이후 `build && 경고 없음 && 시험`으로 묶는다.
- 워치독 재부팅 메시지: Zephyr에서는 printk도 지연 로그를 거친다. 재부팅 직전에는 `LOG_PANIC()`으로 즉시 출력
  모드로 바꿔야 원인이 남는다. 원인이 안 남는 재부팅은 정전과 구분할 수 없다(설계 3단 "고장 구분").
- transactionData는 종료 값을 가장 중요하게 본다(거래를 닫는 값). 종료 값 자리를 먼저 잡고 나머지를 채운다.
  `location`은 기본값(Outlet)이라 생략해서 샘플당 공간을 줄였다.
- 시험이 너무 느슨하면 버그를 통과시킨다: "샘플 0개"도 통과시키던 조건을 "마지막은 반드시 End"로 고쳤다.
