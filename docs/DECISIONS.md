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
