# 단계 1 (junho): 듣기 소켓을 열고 연결 하나를 받아 닫기

참고: CS:APP 11.4 소켓 인터페이스 (팀 Notion 번역본). 코드는 `junho/` 폴더에 있고, 그 안에서 `make`, `make test`, `make asan`을 실행한다. 루트 `docs/step1.md`의 1-1(`socket`)에 이어 `bind`·`listen`·`accept`까지 한 번에 구현했다.

## 무엇을 만들었나

| 파일 | 내용 |
|---|---|
| `src/net.c` `open_listenfd(port)` | CS:APP 그림 11.19와 같은 구조. `getaddrinfo(NULL, port, AI_PASSIVE…)`로 주소 목록을 받아 `socket` → `setsockopt(SO_REUSEADDR)` → `bind`를 성공할 때까지 시도하고, `listen`해서 듣기 fd를 반환. 실패하면 원인 출력 후 -1 |
| `src/net.c` `parse_port(s)` | 명령줄 포트 문자열을 1~65535 정수로 검사. 아니면 -1 |
| `src/main.c` | `./build/server <port>`: 듣기 소켓을 열고 `accept`로 연결 하나를 받아 상대 주소를 출력한 뒤 연결 fd와 듣기 fd를 닫고 종료 |
| `tests/test_net.c` | `parse_port` 경계값, `open_listenfd("0")` → `connect` → `accept`, 이미 listen 중인 포트로 다시 열면 -1 |
| `tests/run_tests.sh` | 서버를 18080번에 띄우고 `curl`로 접속 → curl이 52(빈 응답) 또는 56(RST), 서버가 종료 코드 0인지 확인 |

실행 예:

```
$ cd junho && ./build/server 8080            # 다른 터미널에서: curl http://localhost:8080/
listening on port 8080 (listenfd = 3)
accepted (127.0.0.1, 46632) connfd = 4
```

curl 쪽은 `curl: (52) Empty reply from server`. 요청을 읽지 않고 바로 닫았으니 정상이다 (응답은 단계 3).

## 결정사항과 이유

| 결정 | 이유 |
|---|---|
| `sockaddr_in`을 직접 채우지 않고 `getaddrinfo` 사용 | 책(11.4.7)이 권하는 방식. 주소 구조체·바이트 순서(`htons`)를 직접 다루지 않고, IPv4/IPv6에 상관없이 같은 코드가 동작한다 |
| `getaddrinfo` 실패는 `perror`가 아니라 `gai_strerror` | `getaddrinfo`는 `errno`를 쓰지 않고 에러 코드를 반환값으로 준다 |
| `SO_REUSEADDR` 설정 | 서버를 끈 직후 같은 포트로 다시 켜면 이전 연결이 TIME_WAIT 상태로 남아 `bind`가 실패한다. 개발 중 재시작마다 기다리지 않게 한다 |
| 책의 대문자 래퍼(`Getaddrinfo`, `Close`) 대신 반환값 직접 검사 | `csapp.h`를 가져오지 않았고, 래퍼는 실패하면 프로세스를 끝내 버려 테스트에서 실패 경로를 확인할 수 없다 |
| 포트는 명령줄 인자로 받고 `parse_port`로 검사 | `"80x"`, `"0"`, `"70000"`, `" 80"`, `"+80"`처럼 잘못된 값이 `getaddrinfo`까지 가기 전에 사용법을 출력하고 끝낸다 |
| `getnameinfo`에 `NI_NUMERICHOST`와 `NI_NUMERICSERV` | 도메인 이름 조회(DNS)를 하지 않아 `accept` 직후 출력이 늦어지지 않는다 |
| 단위 테스트는 포트 `"0"` | 커널이 빈 포트를 골라 주므로 다른 프로그램과 포트가 겹쳐 테스트가 깨지지 않는다. 실제 포트는 `getsockname`으로 알아낸다 |

## 중요하다고 본 개념

- **듣기 fd와 연결 fd**: `listen`한 fd(3번)는 연결 요청을 받는 창구이고 서버가 사는 동안 계속 쓴다. `accept`는 연결마다 새 fd(4번)를 만들어 준다. 데이터는 연결 fd로만 주고받는다. 둘을 나눠 두었기 때문에 나중에 연결마다 프로세스나 스레드를 따로 붙일 수 있다(단계 6).
- **`listen` 뒤 연결은 커널이 받는다**: 3-way handshake는 커널이 처리해서 backlog 큐에 쌓아 둔다. 그래서 테스트에서 `accept`를 부르기 전에 `connect`가 먼저 성공한다. `accept`는 큐에서 완성된 연결을 하나 꺼낼 뿐이다.
- **EOF**: 서버가 연결 fd를 `close`하면 클라이언트 쪽 `read`가 0을 반환한다. curl은 응답 한 줄도 받기 전에 EOF를 만나 52로 끝난다.
- **`AI_PASSIVE` + host `NULL`**: 와일드카드 주소(0.0.0.0 또는 ::)가 나온다. 이 컴퓨터의 어느 IP로 들어온 연결이든 받는다.

## 확인한 것

| 확인 | 결과 |
|---|---|
| `make` (`-Wall -Wextra`) | 경고 0 |
| `make test` | 인자 검사 PASS, curl 연결·종료 코드 PASS, 단위 테스트 모두 PASS |
| `make asan` | 같은 테스트 모두 PASS, ASan 보고 없음 |

## 아직 하지 않은 것

- `valgrind --track-fds=yes`: 이 컴퓨터에 valgrind가 없어 확인하지 않았다.
- `socket`·`listen`·`accept`가 실패하는 경로는 일부러 일으키기 어려워 테스트하지 않았다 (`bind` 실패는 포트 충돌 테스트로 확인).
- `run_tests.sh`는 18080번 포트를 쓴다. 그 포트를 다른 프로그램이 쓰고 있으면 실패한다 (세 번째 인자로 바꿀 수 있음).
- `open_listenfd` 포트 충돌 테스트는 Linux 기본값 `net.ipv6.bindv6only = 0`을 전제로 한다. 1이거나 macOS/BSD면 `[::]`에 따로 bind가 되어 실패할 수 있다.
- 연결 하나만 받고 끝난다. 반복 `accept`는 요청을 읽는 단계 2·3에서 붙인다.
