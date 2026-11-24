# Web Server Lab (발표용 데모 페이지)

버튼을 누르면 CGI 프로세스가 웹 서버나 프록시에 실제 요청을 보내고, 받은 응답과 각 프로세스의 로그, 커널의 fd 표를 한 화면에 보여 주는 페이지다.
위쪽의 주제를 누르면 그 주제의 장면만 아래 줄에 펼쳐진다. `구현 전` 표시가 붙은 장면은 제목만 있고, 코드를 구현하면 채운다.

| 주제 | 장면 |
|---|---|
| 소켓과 TCP | Socket과 fd, TCP Handshake와 레이어 |
| HTTP | HTTP GET, 상태 코드(13개), POST, HTTP/1.0과 HTTP/1.1 |
| 동적 요청과 동시성 | 정적과 동적 (CGI), fork 있음과 없음 |
| Reverse Proxy | Reverse Proxy, Round-Robin, 가중치 Round-Robin(구현 전), 502 Bad Gateway, Health Check(구현 전) |
| Forward Proxy | Forward Proxy, 허용 목록 |
| 프록시 부가 기능 | X-Forwarded-For, Response Caching, /status (모두 구현 전) |

## 실행

WSL 터미널에서 레포 맨 위 폴더에서 실행한다.

```bash
bash demo/lab/run.sh
```

브라우저에서 `http://localhost:8000/` 을 연다. 끝낼 때는 그 터미널에서 Ctrl+C를 누른다. 띄운 프로세스가 모두 끝난다.

TCP Handshake와 레이어 장면은 패킷 캡처 프로그램 `tcpdump`가 필요하다. 패킷 캡처는 관리자 권한이 있어야 하므로 아래를 한 번만 실행해 둔다. 하지 않아도 다른 장면은 그대로 동작한다.

```bash
which tcpdump || sudo apt install -y tcpdump
sudo setcap cap_net_raw,cap_net_admin=eip "$(readlink -f "$(which tcpdump)")"
```

## 띄우는 프로세스

| 포트 | 프로세스 | 역할 |
|---|---|---|
| 8000 | `build/server` (폴더 `demo/lab/www`) | 데모 페이지와 CGI 프로그램 `lab`을 서빙한다 |
| 8080 | `build/proxy` (규칙 `demo/lab/proxy.conf`) | 프록시 |
| 8001, 8003 | `build/server` | `/api/` 백엔드 |
| 8002 | `build/server` | `/static/` 백엔드 |
| 8004 | `build/server` (폴더 `www`) | 프록시 없이 직접 요청을 받는 웹 서버. `/cgi-bin/` 백엔드이기도 하다 |
| (포트 없음) | `tcpdump -i lo -l -n -tt -S tcp port 8004` | 포트 8004로 오가는 TCP 패킷을 한 줄씩 적는다 |
| 8005 | `build/server-nofork` (폴더 `www`) | 같은 코드를 `-DNO_FORK`로 빌드한 비교용 웹 서버 |

각 프로세스의 출력은 `demo/lab/run/이름.log`에 쌓이고, 페이지 아래의 터미널 칸이 그 파일을 읽어 보여 준다.

## 화면의 값이 어디서 오는가

| 화면 | 값의 출처 |
|---|---|
| 보낸 요청, 받은 응답, 걸린 시간 | CGI 프로세스 `lab`이 소켓으로 실제로 보내고 받은 바이트와 그 사이에 잰 시간 |
| 터미널 칸 | 각 프로세스가 표준 출력에 쓴 로그 파일 |
| 프록시 그림에서 점이 가는 백엔드 | 프록시 로그의 `[Reverse] GET /api/ → 127.0.0.1:8001` 줄 |
| TCP Handshake와 레이어의 패킷 | `tcpdump`가 포트 8004의 패킷을 적은 `demo/lab/run/tcpdump.log` |
| fd 표 | `/proc/프로세스 번호/fd` (소켓의 상태와 주소는 `/proc/net/tcp`) |
| 켜짐, 꺼짐 표시 | `/proc/프로세스 번호/stat` |
| fork 있음과 없음의 시간 | 8004(fork)와 8005(fork 없음)에 같은 요청 두 개를 보내 잰 값 |

- 요청을 보내는 클라이언트는 브라우저가 아니라 CGI 프로세스 `lab`이다. 브라우저는 Forward Proxy용 요청 줄(`GET http://...`)이나 형식이 틀린 요청(상태 코드 400, 414, 431 등)을 직접 만들 수 없기 때문이다.
- 페이지는 프록시를 거치지 않고 8000에서 직접 연다. 페이지를 주는 프로세스와 실험 대상(프록시, 백엔드)을 나눠 두면, 실험 중에 프록시를 꺼도 페이지가 계속 열린다.

## 데모를 위해 더한 코드

| 위치 | 내용 |
|---|---|
| `proxy/proxy.c`, `proxy/proxy.h` | 설정 파일의 `allow 주소[:포트]` 줄(Forward Proxy 허용 목록). 목록에 없는 서버면 연결하지 않고 403을 보낸다. `allow` 줄이 없으면 전과 같이 모든 서버로 보낸다 |
| `src/main.c` | `#ifdef NO_FORK`: fork하지 않고 웹 서버 프로세스가 연결을 직접 처리하는 비교용 빌드. 기본 빌드(`build/server`)의 동작은 그대로다 |
| `demo/lab/proxy.conf` | 데모용 규칙. `/down/ 127.0.0.1:9`는 서버가 없는 포트로, 502를 보는 용도다 |
| `demo/lab/cgi/slow.c`, `fail.c` | 3초 뒤에 응답하는 CGI 프로그램, 실패로 끝나는 CGI 프로그램(500) |

## 파일

| 파일 | 내용 |
|---|---|
| `demo/lab/run.sh` | 빌드(`make lab`)하고 프로세스 일곱 개를 띄운다 |
| `demo/lab/cgi/lab.c` | CGI 프로그램. `action=send`, `multi`, `fds`, `status`, `stop`, `start`, `log`, `conf` |
| `demo/lab/www/index.html`, `lab.css`, `lab.js` | 페이지. 장면의 글과 버튼은 `lab.js`의 `GROUPS`에 있다 |

## 주의

- 발표용 로컬 데모다. CGI 프로그램 `lab`은 백엔드 프로세스를 끄고 켤 수 있으므로, 다른 사람이 접속할 수 있는 환경에 띄워 두지 않는다.
- 끄기, 켜기 버튼은 `demo/lab/run/포트.pid`에 적힌 프로세스에 SIGTERM을 보낸다. `run.sh`로 띄웠을 때만 동작한다.
- 화면 높이가 560px보다 낮거나 너비가 980px보다 좁으면 한 화면에 넣지 않고 아래로 이어진다.
