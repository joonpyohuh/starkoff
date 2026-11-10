# STARKOFF 과제: QEMU에서 돌아가는 OCPP 1.6J 충전기 펌웨어

OCPP 1.6J(WebSocket 위의 JSON) 충전기(Charge Point) 펌웨어입니다. **Zephyr RTOS v4.2.0** 위에서 QEMU
보드 **`qemu_x86`** 으로 실행됩니다. 일반 리눅스 프로그램이 아니라 MCU급 RTOS 이미지입니다.
실제 충전기가 다루는 바깥 세계(케이블, 차, RFID 카드, 릴레이, 커넥터 잠금, 전력량계, 전원, 네트워크
선, 센서)는 펌웨어 안의 "가짜 하드웨어" 계층으로 흉내 내고, UART 쉘이나 TCP 제어 포트로 조작합니다.

> **현재 상태 (2026-11-10)**: BootNotification · Heartbeat · StatusNotification · Get/ChangeConfiguration이
> 동작하고, 해당 OCTT 시나리오를 우리 실험실에서 통과합니다. **충전(트랜잭션) 기능은 아직 없습니다.**
> 된다고 적은 것은 모두 직접 실행해서 확인한 것입니다. 자세한 내용은 [4. 현재 상태](#4-현재-상태)와
> [docs/TEST_RESULTS.md](docs/TEST_RESULTS.md)를 보세요. 아래 1절의 깨끗한 Ubuntu 22.04 설치 절차는
> **아직 새 PC에서 처음부터 검증하지 않았습니다** (제출 전에 검증 예정).

---

## 1. 깨끗한 Ubuntu 22.04에서 빌드하고 실행하기

### 1.1 시스템 패키지
```bash
sudo apt update
sudo apt install --no-install-recommends -y git cmake ninja-build gperf ccache dfu-util \
  device-tree-compiler wget python3-dev python3-pip python3-setuptools python3-venv \
  python3-wheel xz-utils file make gcc libmagic1 netcat-openbsd
```

### 1.2 작업 공간 (west manifest 저장소)
이 저장소 자체가 west manifest입니다. `west update`는 Zephyr **v4.2.0**과 펌웨어에 필요한 모듈
(`mbedtls`, `picolibc`)만 받습니다.
```bash
mkdir -p ~/starkoff-ws && cd ~/starkoff-ws
python3 -m venv .venv && source .venv/bin/activate
pip install west
git clone https://github.com/joonpyohuh/starkoff.git starkoff
west init -l starkoff
west update --narrow -o=--depth=1
west zephyr-export
pip install -r zephyr/scripts/requirements-base.txt
west sdk install --version 0.17.2 -t x86_64-zephyr-elf   # 툴체인 + 호스트 도구(QEMU 포함)
```
만들어지는 구조 (스크립트가 이 구조를 기준으로 Zephyr를 찾습니다):
```
~/starkoff-ws/
  .venv/      starkoff/ (이 저장소)      zephyr/      modules/
```

### 1.3 빌드
```bash
cd ~/starkoff-ws/starkoff
scripts/build.sh                                            # 기본값: ws://10.0.2.2:9000/ocpp, CP001
scripts/build.sh --url ws://10.0.2.2:9000/ocpp --id CP001   # 기본값을 직접 지정
```
펌웨어는 `<url>/<ChargePointId>`로 접속합니다. QEMU 안에서 호스트 PC의 주소는 `10.0.2.2`입니다.

### 1.4 실험실 도구 설치 (가짜 CSMS + 테스트, 선택)
```bash
scripts/setup_lab.sh              # lab/.venv를 만들고 websockets 패키지 설치
```

### 1.5 실행
```bash
scripts/run_qemu.sh --fresh       # --fresh = 공장 초기 상태로 시작
```
- 이 터미널이 펌웨어의 **UART 쉘**입니다 (프롬프트 `uart:~$`). QEMU 종료: `Ctrl-a` 다음 `x`.
- 호스트 `127.0.0.1:7000`이 펌웨어의 **TCP 제어 포트**로 연결됩니다.
- 설정은 `state/cp_nvm.img`(비휘발 메모리, 5절 참고)에 저장됩니다. `--fresh` 없이 실행하면 재부팅과
  QEMU 재시작 뒤에도 남습니다.

### 1.6 테스트 시나리오 실행
```bash
scripts/run_lab.sh --list                          # 테스트 목록
scripts/run_lab.sh --qemu --all --markdown         # 펌웨어를 직접 띄우고 전부 실행, 결과 표 출력
scripts/run_lab.sh --qemu --test TC_001_CS         # 하나만
```
직접 조작하고 싶다면 터미널 세 개로:
```bash
scripts/run_lab.sh --csms          # 터미널 1: 가짜 CSMS (ws://0.0.0.0:9000/ocpp/<id>), 모든 메시지 출력
scripts/run_qemu.sh --fresh        # 터미널 2: 펌웨어 (ws://10.0.2.2:9000/ocpp/CP001로 접속)
scripts/ctl.sh "plug 1" status     # 터미널 3: 가짜 하드웨어 조작
```
펌웨어는 서버를 못 찾으면 5, 10, 20, 40초 뒤, 그다음엔 60초마다 다시 시도합니다. CSMS를 먼저 켜 두면
부팅 약 2초 뒤에 바로 접속합니다. 나중에 켜면 최대 1분까지 기다릴 수 있습니다.

---

## 2. 서버 주소와 Charge Point ID 바꾸기

| 방법 | 명령 | 저장되나? |
|---|---|---|
| 빌드할 때 기본값 | `scripts/build.sh --url <ws://host:port/path> --id <CP_ID>` | 예. 새로 빌드한 값이 이전 빌드가 저장한 값보다 우선 |
| 실행 중 UART 쉘 | `cp cfg set _ServerUrl ws://10.0.2.2:9000/ocpp`, `cp cfg set _ChargePointId CP001` 후 `cp reboot` | 예 |
| 실행 중 TCP 7000 | `scripts/ctl.sh "cfg set _ServerUrl ..." "cfg set _ChargePointId ..." reboot` | 예 |

WebSocket 주소는 `<url>/<ChargePointId>`, 서브프로토콜은 `ocpp1.6`입니다. `ws://`만 지원합니다 (TLS 없음).

---

## 3. 가짜 하드웨어 제어 명령

UART 쉘(앞에 `cp ` 붙임)과 TCP 7000 포트(한 줄 보내면 한 줄 응답)에서 같은 명령을 씁니다.

| 명령 | 뜻 |
|---|---|
| `status` | 상태 한 줄 (`key=value`: 부팅 상태, 연결, 시계, 전원, 상태등, 케이블, 잠금, 릴레이, 에너지, 고장) |
| `plug <c>` / `unplug <c>` | 케이블 꽂기 / 뽑기 (충전기와 차 양쪽). 잠겨 있으면 `unplug` 거부 |
| `ev_disconnect <c>` | 차 쪽에서만 케이블 분리 |
| `card <c> <idTag>` | RFID 카드 대기 |
| `power off` / `power on` | 전원 차단 / 복구 (복구 시 재부팅) |
| `lock_fail <c> on\|off` | 커넥터 잠금 모터 고장 |
| `fault <c> overcurrent\|overtemp\|overvoltage\|off` | 센서 값을 정상 범위 밖으로 (1초 안에 고장으로 감지) |
| `net_down` / `net_up` | 네트워크 선 뽑기 / 다시 꽂기 |
| `cfg get <k>` / `cfg set <k> <v>` / `cfg list` / `cfg reset` | 설정 키 |
| `reboot` | 재부팅 |

예시:
```bash
scripts/ctl.sh status                          # 한 번 보내고 응답 출력
scripts/ctl.sh "plug 1" "card 1 TAG001" status
nc 127.0.0.1 7000                              # 대화형: 명령을 치면 응답
```
`nc -q`, `nc -N`, `echo 명령 | nc ...`는 쓰지 마세요. 연결을 반쯤 닫는(half-close) 방식이라 Zephyr TCP가
응답을 버립니다 (Zephyr가 TCP half-close를 지원하지 않음, DECISIONS D14).

---

## 4. 현재 상태

| 기능 | 상태 | 확인 방법 |
|---|---|---|
| QEMU `qemu_x86`에서 부팅, e1000 + user-mode 네트워크 | 됨 | 직접 실행 |
| CSMS로 WebSocket 연결 (`ocpp1.6`, PING에 PONG 응답) | 됨 | 가짜 CSMS(20초마다 PING)와 연결 유지 |
| BootNotification Accepted / Pending / Rejected, `interval` 뒤 재전송 | 됨 | TC_001_CS, TC_002_CS, X_REJECTED_SILENT |
| BootNotification.conf의 interval로 Heartbeat | 됨 | TC_001_CS (간격 10.0초) |
| 부팅 시 / 변경 시 커넥터 0, 1의 StatusNotification | 됨 (Available / Preparing / Faulted만) | TC_001_CS, 수동 `plug` / `fault` |
| GetConfiguration / ChangeConfiguration (값 검사 포함) | 됨 | TC_019_CS, TC_021_CS |
| 모르는 메시지 → CALLERROR NotImplemented | 됨 | X_NOTIMPL |
| 오프라인 판정(소켓 끊김, 30초 무응답), 간격을 늘리며 재연결, 재연결 뒤 Boot 다시 안 보냄 | 됨 | X_RECONNECT, CSMS 없이 실행한 로그 |
| `currentTime`으로 UTC 시계 동기화 | 됨 | StatusNotification의 timestamp |
| TCP 제어 포트 + UART 쉘, 재부팅 · QEMU 재시작 뒤 설정 유지 | 됨 | 2단계 점검 |
| 스레드 스택 넘침 감시 (넘치면 즉시 "Stack overflow"로 멈춤) | 켜 둠 | 실제 버그를 이걸로 찾음 (D20) |
| Authorize, Start/StopTransaction, MeterValues, RemoteStart/Stop, Reset, ChangeAvailability, UnlockConnector | **아직 구현 안 됨** | — |
| 오프라인 트랜잭션 큐, 정전 복구, 인증 캐시 | **아직 구현 안 됨** | — |
| 웹 UI | **아직 구현 안 됨** | — |
| 깨끗한 Ubuntu 22.04 절차 (1절) | **아직 검증 안 됨** | 제출 전 예정 |

시험별 결과: [docs/TEST_RESULTS.md](docs/TEST_RESULTS.md)

---

## 5. 알려진 한계
- **비휘발 메모리는 호스트 파일입니다.** QEMU의 `ivshmem` 장치로 1 MiB 파일을 펌웨어 메모리에
  연결합니다 (`qemu_x86`에는 꺼져도 남는 flash가 없음). 실제 충전기라면 flash/FRAM이고, 바뀌는 곳은
  `firmware/src/store/store.c` 하나입니다. DECISIONS D9.
- **Zephyr v4.2.0의 e1000 드라이버가 브로드캐스트 패킷을 받지 않습니다.** 그래서 QEMU에서 DHCP가 안
  됩니다. 펌웨어는 고정 주소 10.0.2.15를 쓰고, 부팅 직후와 30초마다 게이트웨이로 작은 패킷을 보내 QEMU
  NAT가 제어 포트로 들어올 수 있게 합니다. 첫 서버 접속은 NAT가 준비되는 부팅 약 2초 뒤에 합니다. D7.
- QEMU 화면에 가끔 `Slirp: Failed to send packet, ret: -1`이 보입니다. e1000 드라이버가 받지 않은
  패킷(브로드캐스트, 또는 수신 버퍼가 하나뿐이라 바쁠 때 온 패킷)을 QEMU가 알려주는 것이고, TCP가
  재전송합니다. 펌웨어 고장이 아닙니다.
- 그 수신 버퍼 때문에 메시지가 가끔 1초 정도 늦게 도착합니다. TC_001에서 한 번, 펌웨어 로그상
  정확히 10.0초에 보낸 Heartbeat가 CSMS에는 10.8초에 도착했습니다.
- 가짜 하드웨어는 "전원이 꺼진" 동안에도 동작합니다 (바깥 세계 역할이라 누군가 전원을 다시 켤 수
  있어야 함). 멈추는 것은 충전기 로직뿐입니다. D12.

---

## 6. 내가 만든 것과 AI 사용
- **저(작성자)**: 요구사항, 범위와 시험 우선순위, 오프라인 · 정전 · 고장 설계
  ([DESIGN_resilience.md](DESIGN_resilience.md)), 기본 기능 설계 ([DESIGN_core_v1.md](DESIGN_core_v1.md)),
  설계 결정, 단계마다 검토와 지시.
- **AI (Claude Code)**: 제 지시에 따라 코드와 문서 대부분을 작성하고, 빌드 · QEMU 테스트를 실행하고,
  문제를 디버깅했습니다. 단계별 기록은 [docs/AI_USAGE.md](docs/AI_USAGE.md)에 있습니다.
- **사용한 오픈소스**: Zephyr RTOS(RTOS, TCP/IP, WebSocket 클라이언트, 쉘), jsmn v1.1.0(JSON 토크나이저,
  MIT, `firmware/lib/jsmn/`), 파이썬 `websockets` 12.0(가짜 CSMS 전용).
- 설계 결정과 이유: [docs/DECISIONS.md](docs/DECISIONS.md)

## 7. 저장소 구조
```
firmware/   Zephyr 앱 (C) — 실제 제출물
  src/ocpp/   OCPP 로직, 설정 키, 시계, JSON
  src/net/    WebSocket 연결, SLIRP 알림
  src/sim/    가짜 하드웨어
  src/ctrl/   UART 쉘 + TCP 제어 포트
  src/store/  비휘발 기록 저장
  lib/jsmn/   JSON 토크나이저 (외부, MIT)
lab/        호스트 도구 (가짜 CSMS, 테스트, 이후 웹 UI) — 펌웨어에 포함되지 않음
  csms/       가짜 Central System (OCPP 1.6J WebSocket 서버)
  tests/      OCTT 시나리오(core.py)와 추가 점검(extra.py, X_로 시작)
scripts/    build.sh, run_qemu.sh, ctl.sh, setup_lab.sh, run_lab.sh
docs/       DECISIONS.md, AI_USAGE.md, TEST_RESULTS.md, reference/ (규격, OCTT 시험 문서, 과제 설명서)
west.yml    Zephyr v4.2.0 고정 + 모듈 허용 목록
```

## 8. 외부 자료
- Zephyr RTOS v4.2.0 (Apache-2.0), mbedtls, picolibc — `west update`로 받음, 저장소에 복사하지 않음.
- jsmn v1.1.0 (MIT) — `firmware/lib/jsmn/`에 원본 그대로 LICENSE와 함께 포함.
- 파이썬 `websockets` 12.0 (BSD) — `scripts/setup_lab.sh`가 설치, 실험실 전용.
- `docs/reference/`: OCPP 1.6 Edition 2 규격과 OCTT 시험 문서 © Open Charge Alliance, 참고용 원본.
