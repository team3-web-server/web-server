# junho/tiny/: CS:APP 11.4~11.6 예제 코드

책(CS:APP 3판) 11장 예제를 팀 Notion 번역본 코드를 바탕으로 옮기고, 함수 이름과 동작은 그대로 둔 채 읽기 쉽게 다시 썼다. `junho/`의 단계별 서버와는 따로 빌드한다.

| 파일 | 책 그림 | 내용 |
|---|---|---|
| `hostinfo.c` | 11.17 | 도메인 이름에 대응하는 IPv4 주소 출력 |
| `echoclient.c` | 11.20 | 표준 입력 줄을 서버에 보내고 되돌아온 줄 출력 |
| `echoserveri.c`, `echo.c` | 11.21, 11.22 | 반복 에코 서버 |
| `cgi-bin/adder.c` | 11.27 | 두 수를 더하는 CGI 프로그램 |
| `tiny.c` | 11.29~11.35 | Tiny 웹 서버 (정적·동적 콘텐츠, GET만 지원) |
| `netlib.h`, `netlib.c` | 10.5, 11.4.8 | 책 라이브러리 `csapp.h`/`csapp.c` 중 위 프로그램이 쓰는 것만 직접 다시 구현한 것. 함수 이름은 책과 같다 (Rio, `Open_listenfd`·`Open_clientfd`, `Accept`, `Fork` 같은 대문자 래퍼) |
| `home.html`, `godzilla.gif` | - | Tiny의 기본 페이지와 이미지. 책 배포본 파일이 아니라 새로 만든 것 |

## 빌드와 실행

```
cd junho/tiny
make           # tiny, cgi-bin/adder, echoclient, echoserveri, hostinfo
make test      # tests.sh: Tiny·adder·에코·hostinfo 동작 확인
```

Tiny는 정적 파일을 현재 디렉터리에서 찾으므로 `junho/tiny/` 안에서 실행한다.

```
./tiny 8000
# 브라우저: http://localhost:8000/                     → home.html과 이미지
#           http://localhost:8000/cgi-bin/adder?15000&213 → 15000 + 213 = 15213
```

에코:

```
./echoserveri 9000          # 터미널 1
./echoclient localhost 9000 # 터미널 2: 입력한 줄이 그대로 돌아옴, Ctrl+D로 종료
./hostinfo localhost        # 127.0.0.1
```

## 책 코드와 다른 점

| 위치 | 바꾼 것 | 이유 |
|---|---|---|
| 모든 프로그램 | `#include "csapp.h"` → `#include "netlib.h"` | 책 라이브러리 전체(시그널·스레드·세마포어 래퍼 등 약 1000줄) 대신 필요한 함수만 담은 `netlib`을 쓴다 |
| `netlib.c` 대문자 래퍼 | 실패 시 `exit(0)` → `exit(1)` | 원본은 실패해도 종료 코드가 0이라 스크립트·테스트가 실패를 알 수 없다 |
| 모든 `.c` | 함수 이름·인자·출력은 그대로 두고 본문을 읽기 쉽게 다시 씀 (대입과 비교를 나누고, 중괄호를 항상 쓰고, 변수 이름을 하는 일대로) | 고치기 전 버전과 Tiny 응답 7종(`/`·`/home.html`·GIF·adder·404·403·501)을 바이트 단위로 비교해 같음을 확인했다 |
| `tiny.c` `serve_static`, `clienterror`, `cgi-bin/adder.c` | `sprintf(buf, "%s...", buf)`로 조금씩 이어 붙이던 문자열을 `snprintf` 한 번으로 만듦 | 책 방식은 원본과 대상이 겹쳐 정의되지 않은 동작이다. 책 그대로 `-O2`로 빌드하면 Ubuntu gcc의 `_FORTIFY_SOURCE` 때문에 앞 내용이 지워져, 정적 응답 헤더가 `Content-type` 한 줄만 나가고 adder 본문은 `Thanks for visiting!`만 남는 것을 확인했다 |
| `tiny.c` `read_requesthdrs` | 첫 헤더 줄도 서버 로그에 출력 | 책 코드는 첫 헤더 줄을 읽고 출력하지 않는다. 클라이언트에게 가는 응답은 같다 |
| `tiny.c` `clienterror` | 본문이 `MAXBUF`(8192바이트)를 넘으면 잘라서 보냄 | 책 `sprintf`는 아주 긴 URI(약 8100바이트)에서 스택 버퍼를 넘어 쓴다. `snprintf`는 잘린 본문과 그 길이에 맞는 `Content-length`를 보낸다 |
| `cgi-bin/adder.c` | 쓰이지 않던 `sprintf(content, "QUERY_STRING=%s", buf)` 줄 삭제 | 바로 다음 줄이 덮어써서 출력에 영향이 없다 |

## 책 코드의 한계 (고치지 않음)

- **빈 연결에 Tiny가 죽는다**: 요청 없이 연결했다가 닫으면 `doit`이 요청 줄을 못 읽은 채 에러 응답을 쓰다가, 이미 닫힌 연결에 써서 `SIGPIPE`로 프로세스가 종료된다(책 11.6 마지막 Aside). `tests.sh`가 서버 준비를 빈 연결이 아니라 실제 요청으로 확인하는 이유다.
- **헤더가 빈 줄로 끝나지 않으면 무한 루프**: `read_requesthdrs`는 EOF를 검사하지 않는다. 이 판은 루프를 돌 때마다 마지막 줄을 서버 로그에 계속 출력한다.
- **`adder`에 `&` 없는 인자를 주면 크래시**: `strchr`가 NULL을 돌려줘도 `*amp`에 쓴다.
- **경로 검사 없음**: `/../` 같은 URI로 상위 디렉터리 파일을 읽을 수 있다. 게다가 `parse_uri`는 URI 어디에든 `cgi-bin`이 있으면(쿼리 문자열 포함) 동적 콘텐츠로 보므로, `GET /../../../../usr/bin/id?cgi-bin`처럼 `../`로 닿는 아무 실행 파일이나 실행된다(리뷰에서 재현). 외부에 열어 두면 안 된다. 팀 로드맵 단계 5에서 다룬다.
