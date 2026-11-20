# tiny/: CS:APP 11.4~11.6 예제 코드

책(CS:APP 3판) 11장 예제를 팀 Notion 번역본 코드 그대로 옮겼다. 팀 로드맵의 서버(`src/`)와는 따로 빌드한다.

| 파일 | 책 그림 | 내용 |
|---|---|---|
| `hostinfo.c` | 11.17 | 도메인 이름에 대응하는 IPv4 주소 출력 |
| `echoclient.c` | 11.20 | 표준 입력 줄을 서버에 보내고 되돌아온 줄 출력 |
| `echoserveri.c`, `echo.c` | 11.21, 11.22 | 반복 에코 서버 |
| `cgi-bin/adder.c` | 11.27 | 두 수를 더하는 CGI 프로그램 |
| `tiny.c` | 11.29~11.35 | Tiny 웹 서버 (정적·동적 콘텐츠, GET만 지원) |
| `csapp.h`, `csapp.c` | 10.5, 11.4.8 | 위 프로그램이 쓰는 책 라이브러리 함수만 직접 작성 (Rio, `open_clientfd`, `open_listenfd`, 대문자 래퍼) |
| `home.html`, `godzilla.gif` | - | Tiny의 기본 페이지와 이미지. 책 배포본 파일이 아니라 새로 만든 것 |

## 빌드와 실행

```
cd tiny
make           # tiny, cgi-bin/adder, echoclient, echoserveri, hostinfo
make test      # tests.sh: Tiny·adder·에코·hostinfo 동작 확인
```

Tiny는 정적 파일을 현재 디렉터리에서 찾으므로 `tiny/` 안에서 실행한다.

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
| `tiny.c` `serve_static`, `clienterror`, `cgi-bin/adder.c` | `sprintf(buf, "%s...", buf, ...)`를 `sprintf(buf + strlen(buf), "...", ...)`로 | 출력 버퍼를 `%s` 인자로도 넘기면 원본과 대상이 겹쳐 정의되지 않은 동작이다. 책 그대로 `-O2`로 빌드하면 Ubuntu gcc가 켜는 `_FORTIFY_SOURCE` 때문에 앞 내용이 지워져, 정적 응답 헤더가 `Content-type` 한 줄만 나가고 adder 본문은 `Thanks for visiting!`만 남는 것을 확인했다. 줄 순서와 출력 내용은 책과 같다 |
| `csapp.c` | 책 라이브러리 전체가 아니라 필요한 함수만 | 원본 파일을 가져오지 않고 직접 작성했다. 시그널·스레드·세마포어 래퍼는 없다. 동작은 원본과 같게 맞췄다(래퍼는 실패하면 메시지 출력 후 `exit(0)`) |

## 책 코드의 한계 (고치지 않음)

- **빈 연결에 Tiny가 죽는다**: 요청 없이 연결했다가 닫으면 `doit`이 요청 줄을 못 읽은 채 에러 응답을 쓰다가, 이미 닫힌 연결에 써서 `SIGPIPE`로 프로세스가 종료된다(책 11.6 마지막 Aside). `tests.sh`가 서버 준비를 빈 연결이 아니라 실제 요청으로 확인하는 이유다.
- **헤더가 빈 줄로 끝나지 않으면 무한 루프**: `read_requesthdrs`는 EOF를 검사하지 않는다.
- **`adder`에 `&` 없는 인자를 주면 크래시**: `strchr`가 NULL을 돌려줘도 `*p`에 쓴다.
- **경로 검사 없음**: `/../` 같은 URI로 상위 디렉터리 파일을 읽을 수 있다. 게다가 `parse_uri`는 URI 어디에든 `cgi-bin`이 있으면(쿼리 문자열 포함) 동적 콘텐츠로 보므로, `GET /../../../../usr/bin/id?cgi-bin`처럼 `../`로 닿는 아무 실행 파일이나 실행된다(리뷰에서 재현). 외부에 열어 두면 안 된다. 팀 로드맵 단계 5에서 다룬다.
- **`make` 경고 1개**: `serve_static`의 `Content-type: %s` 줄에 대한 `-Wformat-overflow` 경고. `filetype`은 `text/html` 같은 짧은 문자열이라 실제로 넘치지 않는다.
