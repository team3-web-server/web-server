# web-server

CS:APP 11장 Tiny를 바탕으로 만든 HTTP/1.0 웹 서버. TCP 소켓으로 연결을 받고, HTTP 요청을 파싱해 정적 파일이나 CGI 응답을 보낸다.

## 구조

| 경로 | 내용 |
|---|---|
| `src/server_main.c` | `main`, accept 루프, SIGCHLD·SIGPIPE 처리 |
| `src/http.c`, `include/http.h` | 요청 줄·헤더·본문 파싱, 오류 응답 |
| `src/serve.c`, `include/serve.h` | 정적 파일, CGI 실행 |
| `src/netlib.c`, `include/netlib.h` | Rio I/O, `Open_listenfd`, 책 스타일 래퍼 |
| `www/` | 문서 루트. `home.html`, `godzilla.gif`, `sample.mpg`, `adder.html`, `cgi-bin/` |
| `tests/server_test.sh` | 정적·CGI·HEAD·POST·오류·SIGPIPE 통합 테스트 |

## 빌드와 실행

```
make
cd www
../build/server 8000
```

브라우저나 `curl`로 연다.

```
http://localhost:8000/                    # home.html
http://localhost:8000/adder.html          # 덧셈 폼
http://localhost:8000/cgi-bin/adder?1&2   # 1 + 2 = 3
curl -X POST --data-binary 'hello' http://localhost:8000/cgi-bin/postecho
```

## 테스트

```
make test
```

서버를 `www/`에서 띄우고 `curl`·`nc`로 다음을 확인한다.

- 정적 파일 본문·MIME(`text/html`, `image/gif`, `video/mpeg`)·0바이트·큰 파일
- CGI adder(GET 폼과 `1&2`), POST 본문 에코
- HEAD(정적·CGI·404), 400·403·404·405·413·414·431·501·505
- CGI 자식 회수(SIGCHLD), 조기 종료 뒤 서버 생존(SIGPIPE/EPIPE)

## 지원 범위

- 메서드: `GET`, `HEAD`, `POST`
- 정적 파일: `www/` 기준, `Content-Type`/`Content-Length`/`Connection: close`
- 동적: `/cgi-bin/` CGI (`fork`+`execve`, GET/HEAD/POST, `tmpfile`로 본문 전달)
- 오류: 400, 403, 404, 405, 413, 414, 431, 500, 501, 505

## 아직 없는 것

- **동시 요청 처리**: 현재 한 연결을 끝낸 뒤 다음 `accept`를 한다. 과제 필수 항목이라 thread/poll 방식 중 하나를 추가해야 한다.
- Reverse Proxy: 동적 요청은 CGI로 처리한다.
- HTTP/1.1 keep-alive, TLS, 압축, 디렉터리 목록.
- 퍼센트 인코딩(`%2e%2e`)·심볼릭 링크 경로 탈출 방어.
