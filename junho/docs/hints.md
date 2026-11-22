# 구현 힌트: 필수 항목

과제 필수 구현 항목을 `junho/src`에서 구현할 때 막히기 쉬운 지점과 최소 구현 방향을 정리했다. 기준은 2026-11-22의 코드이고, 근거 파일을 함께 적었다. 세부 동작은 `src/tiny.c`(책 Tiny)와 `src/net.c`(단계 1)를 먼저 읽는 것을 전제로 한다.

## 0. 공통 함정

| 함정 | 증상 | 대응 |
|---|---|---|
| SIGPIPE | 클라이언트가 먼저 끊으면 서버가 종료됨 (`tiny/README.md`에 기록) | 시작 시 `signal(SIGPIPE, SIG_IGN)` 또는 `send(MSG_NOSIGNAL)` |
| fd 누수 | 연결마다 `close(connfd)`를 빠뜨려 결국 `EMFILE` | 모든 반환 경로에서 close. `valgrind --track-fds=yes`로 확인 |
| `../` 경로 | 문서 루트 밖 파일을 읽음 (`tiny/README.md`에 재현 기록) | 퍼센트 디코딩 후 `realpath`, 루트 접두사 검사 |
| `Content-Length`+`Transfer-Encoding` | 요청 smuggling | 둘 다 있으면 400 |
| keep-alive 프레이밍 | 다음 요청의 시작을 본문으로 오해 | 본문을 정확히 읽은 뒤에만 다음 요청 파싱 |
| 0바이트 파일 | `mmap` 길이 0 → `EINVAL` → 서버 종료 | `filesize == 0`이면 헤더만 보내고 끝 |
| 느린 클라이언트 | 순차 accept 루프 전체가 멈춤 | `SO_RCVTIMEO` 또는 `poll`/`epoll` |

## 1. TCP 소켓 서버

- 이미 있는 것: `src/net.c`의 `open_listenfd`(`getaddrinfo` → `socket` → `SO_REUSEADDR` → `bind` → `listen`), `src/step1_main.c`의 accept 1회, `src/tiny.c`의 반복 accept.
- 남은 것: 요청/응답 연결, `EINTR` 재시도, `EMFILE` 백오프, SIGPIPE 처리.
- 힌트:
  - `accept`가 -1이면 `errno == EINTR`만 재시도하고, 나머지는 로그 후 루프 계속.
  - `EMFILE`/`ENFILE`이면 `sleep(1)` 정도 두고 재시도. tight loop 금지.
  - `listenfd`는 루프 밖에서 한 번만 열고 종료 시 닫기.
- 확인:
  ```
  make && make test
  ./build/server 8080        # 다른 터미널: curl -v http://127.0.0.1:8080/
  ss -lnt 'sport = :8080'
  ```

## 2. 요청 줄 파싱

- 형식: `method SP request-target SP HTTP-version CRLF`. 조각은 정확히 3개.
- `src/tiny.c:57`의 `sscanf`는 조각 수·길이를 검사하지 않으므로 교체 대상.
- 힌트:
  - `\r\n`을 먼저 잘라내고, 공백(`SP`) 위치를 찾아 세 조각으로 나눈다.
  - 각 조각은 고정 버퍼에 **길이 검사 후** 복사. 조각이 3개가 아니면 400.
  - 버전은 `HTTP/1.0`·`HTTP/1.1`만 허용, 아니면 505.
  - URI가 `MAXLINE`을 넘으면 414.
- 확인:
  ```
  printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | nc 127.0.0.1 8080
  printf 'GET /only-two\r\n\r\n' | nc 127.0.0.1 8080    # 400
  ```

## 3. 헤더 파싱

- 빈 줄이 헤더 끝. `\r\n`과 `\n`을 모두 종료로 인정하고 **EOF도 종료**로 처리해야 한다.
- 이름은 대소문자 무시, 값 앞뒤 OWS(공백/탭) 제거.
- 전부 저장하지 말고 `Host`, `Content-Length`, `Connection`만 구조체에 담는다.
- 힌트:
  - HTTP/1.1에서 `Host`가 없으면 400.
  - `Content-Length`가 두 번 나오면 400. `Transfer-Encoding`과 함께 있어도 400.
  - 헤더 총량(예: 8KB)과 줄 길이에 상한을 두고 넘으면 431.
- 확인: `curl -H 'Host:'`, 중복 CL, CL+TE를 각각 보내 상태 코드 확인.

## 4. 본문 읽기

- `Content-Length: N`이면 정확히 N바이트를 읽어야 다음 요청 경계가 맞는다.
- 힌트:
  - `strtoul`로 파싱하고 `end`가 문자열 끝인지 확인. 음수·문자·오버플로는 400.
  - `read`는 한 번에 다 오지 않을 수 있으므로 N바이트가 찰 때까지 루프.
  - 본문 상한(예: 1MB)을 넘으면 413.
  - 버퍼에 남은 바이트는 버리지 말고 다음 요청 파싱에 넘긴다.
- 확인: 본문이 있는 POST 뒤에 같은 연결로 GET을 보내 두 응답이 나오는지.

## 5. 응답 생성

- 응답은 한 함수로 모은다: 상태 줄 + 헤더 + 본문.
- 힌트:
  - 상태 줄은 `HTTP/1.1 <code> <reason>`.
  - `Content-Length`는 항상 실제 본문 바이트 수. 본문이 없으면 `0`.
  - `Connection: close` 또는 `keep-alive`를 요청과 일치시킨다.
  - 파일 open/mmap 실패는 500으로 응답하고 서버는 계속 산다.
  - 미지원 메서드는 405 + `Allow: GET, POST`.
- 확인: `curl -i`에서 상태 줄·`Content-Length`·`Connection` 확인.

## 6. 정적 파일 서빙

- 문서 루트는 `argv` 또는 환경변수로 받는다. 지금은 cwd 고정(`src/tiny.c`의 `parse_uri`).
- 힌트:
  - URI를 퍼센트 디코딩한 뒤 `realpath`로 정규화하고, 루트 경로의 접두사인지 검사.
  - MIME는 **마지막 `.` 기준 확장자**로 찾는다. `strstr`는 `a.html.txt`를 잘못 분류.
  - 0바이트 파일은 `mmap`하지 말고 헤더만 보낸다.
  - 권한은 소유자 비트만 보지 말고 읽기 비트(0444)를 본다.
- 확인:
  ```
  curl --path-as-is http://127.0.0.1:8080/../etc/passwd   # 400/403
  curl -i http://127.0.0.1:8080/style.css                 # text/css
  ```

## 7. 동적 요청 처리 (CGI)

- 지금은 `QUERY_STRING`만 넘기고 본문은 넘기지 않는다(`src/tiny.c:163`).
- 힌트:
  - `REQUEST_METHOD`, `CONTENT_LENGTH`, `CONTENT_TYPE`, `SCRIPT_NAME`, `SERVER_PROTOCOL`을 환경변수로 설정.
  - POST 본문은 자식의 stdin으로 전달. 부모는 `dup2` 후 원래 fd를 정리.
  - 자식에서 `listenfd`를 닫거나 `FD_CLOEXEC`를 설정.
  - CGI 출력의 헤더를 파싱하고, `Content-Length`가 없으면 그 응답만 `Connection: close`.
  - `waitpid`에 타임아웃을 두어 무한 대기 CGI가 서버를 멈추지 않게 한다.
- 확인: `curl 'http://127.0.0.1:8080/cgi-bin/adder?1&2'`, `curl -X POST --data-binary @body ...`.

## 8. 동시 요청 처리

- 지금은 순차 루프라 한 연결이 끝나야 다음 `accept`(`src/tiny.c:27`).
- 힌트:
  - 코드가 가장 적은 선택: 연결마다 `pthread_create` + `detach` + `SO_RCVTIMEO`.
  - 스레드 수 상한을 두고, 초과 시 503 또는 연결 종료 정책을 정한다.
  - 로그·설정 같은 공유 자원은 mutex로 보호.
  - keep-alive를 켜면 연결이 오래 살아 스레드가 묶이므로 타임아웃이 필수.
  - `fork`는 연결당 프로세스 비용이 크고, `epoll`은 상태 머신 작성량이 크다.
- 확인:
  ```
  ab -n 1000 -c 50 http://127.0.0.1:8080/
  ```

## 9. 테스트 명령

```
cd junho
make                 # build/server, build/tiny
make test            # 소켓 단위·기능 테스트
make tiny-test       # Tiny HTTP 동작 테스트
make asan            # ASan
bash valgrind.sh     # 메모리·fd 검사 (valgrind 3.26.0)
```

## 10. 코드 지도

| 파일 | 역할 |
|---|---|
| `src/net.c` | `open_listenfd`, `parse_port` |
| `src/step1_main.c` | 단계 1 서버 (accept 1회) |
| `src/tiny.c` | Tiny HTTP 서버 (`doit`, `parse_uri`, `serve_static`, `serve_dynamic`) |
| `src/netlib.c` | Rio 버퍼 읽기, 대문자 래퍼 |
| `tests/test_net.c` | 소켓 단위 테스트 |
| `tiny/tests.sh` | HTTP 기능 테스트 |

## 11. 작업 순서 제안

1. 요청 줄 파서 → 2. 헤더 저장 → 3. 본문 읽기 → 4. 응답 빌더 → 5. 정적 파일 → 6. keep-alive → 7. CGI 확장 → 8. 동시성 → 9. SIGPIPE·accept 마무리.

3번(본문 읽기)이 끝나기 전에 keep-alive를 켜면 프레이밍이 깨진다.
