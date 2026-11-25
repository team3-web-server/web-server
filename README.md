# web-server

C와 POSIX 소켓 API로 직접 만든 **웹 서버**와 **프록시**입니다. 웹 서버는 정적 파일과 CGI 결과를 HTTP로 돌려주고, 프록시는 여러 대의 웹 서버 앞에서 요청을 나눠 보냅니다.

## 팀

<table align="center">
  <tr>
    <td align="center" width="50%">
      <a href="https://github.com/ksomang"><img src="https://github.com/ksomang.png?size=200" width="150" alt="김소망"></a><br><br>
      <a href="https://github.com/ksomang"><b>김소망</b></a>
    </td>
    <td align="center" width="50%">
      <a href="https://github.com/NARi3040"><img src="https://github.com/NARi3040.png?size=200" width="150" alt="박준호"></a><br><br>
      <a href="https://github.com/NARi3040"><b>박준호</b></a>
    </td>
  </tr>
</table>

## 목차

1. [웹 서버 한 대를 띄우면 끝일까?](#1-웹-서버-한-대를-띄우면-끝일까)
2. [실행 화면](#2-실행-화면)
3. [직접 실행](#3-직접-실행)
4. [아직 없는 것](#4-아직-없는-것)

## 1. 웹 서버 한 대를 띄우면 끝일까?

웹 서버의 기본은 단순합니다. 연결을 받고(`accept`), 요청을 읽고, 파일을 찾아 보내면 됩니다. 사용자가 한 명이면 이것으로 충분합니다. **그런데 요청이 여러 개가 되고 서버가 여러 대가 되면 무슨 일이 생길까요?**

![장면 1 느린 연결 하나가 모두를 기다리게 함](docs/images/readme-scene-1.svg)

![장면 2 요청마다 연결을 새로 맺음](docs/images/readme-scene-2.svg)

![장면 3 서버 한 대에 요청이 몰림](docs/images/readme-scene-3.svg)

![장면 4 백엔드 하나가 죽어도 계속 보냄](docs/images/readme-scene-4.svg)

![장면 5 같은 응답을 매번 다시 만듦](docs/images/readme-scene-5.svg)

다섯 장면을 이렇게 풀었습니다.

| 장면 | 해결 방법 | 자세히 |
| --- | --- | --- |
| ① 느린 연결이 모두를 막음 | 웹 서버는 연결마다 `fork`로 자식 프로세스를, 프록시는 연결마다 스레드를 만듭니다 | [단계 9·10](docs/step9-10.md), [단계 14](docs/step14.md) |
| ② 매번 새 연결 | keep-alive로 연결 하나에서 요청 여러 개를 처리합니다. 프록시와 백엔드 사이의 연결도 닫지 않고 보관했다가 다시 씁니다 | [단계 15](docs/step15.md), [단계 19](docs/step19.md) |
| ③ 한 대에 몰림 | 클라이언트는 프록시만 알면 됩니다. 프록시가 백엔드를 차례대로 골라 보냅니다(라운드로빈) | [단계 11~13](docs/step11-13.md) |
| ④ 죽은 백엔드 | 헬스 체크로 꺼진 백엔드를 빼고, 연결에 실패하면 바로 다음 백엔드로 다시 보냅니다(failover) | [단계 16](docs/step16.md) |
| ⑤ 같은 응답 반복 | 설정한 경로의 응답을 프록시 메모리에 10초 동안 두고 다시 씁니다 | [단계 17](docs/step17.md), [단계 18](docs/step18.md) |

![전체 구조: 클라이언트, Reverse Proxy, 백엔드 웹 서버 세 대](docs/images/readme-architecture.svg)

### 웹 서버 (`build/server`)

- `GET`과 `POST`를 받습니다. 그 밖의 메서드는 501입니다.
- 정적 파일은 지정한 폴더(기본 `./www`)에서 읽어 확장자에 맞는 `Content-Type`과 `Content-Length`를 붙여 보냅니다. 경로의 한 칸이 `..`이면(예: `/a/../b`) 403입니다.
- `/cgi-bin/`으로 시작하는 경로는 프로그램을 실행합니다. 요청 정보는 환경 변수로, POST 본문(최대 1MiB)은 파이프를 통해 표준 입력으로 넘기고, 표준 출력을 받아 응답을 만듭니다.
- 다음 요청이 5초 동안 없으면 연결을 닫습니다.
- `Ctrl+C`(`SIGINT`)나 `SIGTERM`을 받으면 새 연결을 받지 않고, 처리 중인 연결이 끝난 뒤 종료합니다.

### 프록시 (`build/proxy`)

- **Reverse Proxy**: [`proxy.conf`](proxy.conf)의 경로 접두사로 백엔드를 고릅니다. 맞는 규칙이 없으면 404, 살아 있는 백엔드가 없으면 503입니다.
- **Forward Proxy**: 요청 대상이 `http://`로 시작하면 클라이언트가 적은 서버로 보냅니다. 연결하지 못하면 502입니다.
- **헬스 체크**: 모든 백엔드에 `connect`해 보고 2초 쉬기를 반복합니다.
- **캐시**: `cache` 접두사에 맞는 `GET`의 200 응답을 저장합니다. 사본 하나는 100KiB, 전체는 1MiB까지입니다.
- **연결 보관함**: 백엔드마다 쉬는 연결을 4개까지 보관하고, 본문 없는 `GET`에 다시 씁니다.

## 2. 실행 화면

![브라우저로 웹 서버를 연 화면: team3 웹 서버 제목, 자바스크립트 실행 결과, PNG와 JPG 이미지](docs/images/readme-demo-page.png)

웹 서버를 띄우고 브라우저로 연 화면입니다. HTML, CSS, JavaScript, PNG, JPG 파일을 각각 알맞은 `Content-Type`으로 보내야 이렇게 보입니다.

![프록시에 요청을 보내 라운드로빈, 캐시, 헬스 체크를 확인한 실제 출력](docs/images/readme-run.svg)

프록시에 요청을 보낸 실제 출력입니다. `/api/`는 8001과 8003이 번갈아 응답하고, `/static/`은 두 번째부터 캐시에서 나가며, 8001을 끄면 8003만 응답합니다.

## 3. 직접 실행

Linux(또는 WSL)에 `gcc`, `make`, `curl`이 있어야 합니다.

### 1단계: 코드 받고 빌드하기

```bash
git clone https://github.com/team3-web-server/web-server.git
cd web-server
make
```

`build/server`, `build/proxy`, CGI 예제 프로그램(`www/cgi-bin/hello`, `www/cgi-bin/echo`)이 만들어집니다. 이후 명령은 모두 `web-server` 폴더에서 입력합니다.

### 2단계: 웹 서버 띄우기

```bash
./build/server
```

8080번 포트에서 `./www` 폴더를 보여 줍니다. 포트와 폴더는 `./build/server 9000 ./www`처럼 바꿀 수 있습니다. 브라우저에서 `http://localhost:8080/`을 열면 위 실행 화면이 나옵니다.

### 3단계: 새 터미널에서 요청 보내기

```bash
curl -i http://localhost:8080/
```

`index.html`을 응답 헤더까지 보여 줍니다.

```bash
curl -i 'http://localhost:8080/cgi-bin/hello?name=kim'
```

CGI 프로그램이 `QUERY_STRING=name=kim`과 자기 프로세스 번호, 시각을 돌려줍니다.

```bash
curl -X POST --data-binary 'hello' http://localhost:8080/cgi-bin/echo
```

보낸 본문 `hello`와 길이 `CONTENT_LENGTH=5`를 돌려줍니다.

```bash
curl -i http://localhost:8080/nope.html
```

`404 Not Found`가 돌아옵니다. 확인이 끝나면 서버 터미널에서 `Ctrl+C`를 눌러 끕니다. 다음 단계의 프록시가 같은 8080 포트를 씁니다.

### 4단계: 백엔드 세 대와 프록시 띄우기

터미널 네 개에 하나씩 실행합니다.

```bash
./build/server 8001 demo/proxy/backend1    # 터미널 1: /api/
./build/server 8002 demo/proxy/backend2    # 터미널 2: /static/
./build/server 8003 demo/proxy/backend3    # 터미널 3: /api/
./build/proxy 8080 proxy.conf              # 터미널 4: 프록시
```

### 5단계: 프록시에 요청 보내기

다섯 번째 터미널에서 입력합니다. 어느 백엔드로 갔는지, 캐시를 썼는지는 프록시 터미널에도 출력됩니다.

```bash
curl http://localhost:8080/api/       # 두 번 실행: 8001, 8003이 번갈아 응답
curl http://localhost:8080/static/    # 두 번 실행: 두 번째는 [Cache] HIT
curl -i http://localhost:8080/other   # 404 (맞는 규칙 없음)
curl -x http://localhost:8080 http://localhost:8002/static/   # Forward Proxy로 보내기
```

이어서 터미널 1의 백엔드를 `Ctrl+C`로 끄고 몇 초 뒤 `/api/`를 요청하면 8003만 응답합니다. 터미널 3까지 끄면 `503 Service Unavailable`이 돌아옵니다.

### 6단계: 테스트 실행하기

```bash
make test    # 웹 서버 테스트와 프록시 테스트
make asan    # 같은 테스트를 AddressSanitizer·LeakSanitizer로
```

테스트마다 `PASS` 또는 `FAIL`이 한 줄씩 나옵니다. 테스트는 18000번대 포트를 씁니다.

> curl 8.18.0에서는 연결 재사용을 확인하는 검사가 웹 서버와 프록시에 하나씩 `FAIL`이 납니다. 테스트는 curl 출력에서 `Re-using existing`을 찾는데, 이 버전은 `Reusing existing`이라고 출력하기 때문입니다. `make test`와 `make asan`은 웹 서버 테스트에서 멈추므로, 프록시 테스트는 `bash tests/run_proxy_tests.sh build/proxy build/server build/test_proxy`로 따로 실행합니다.

## 4. 아직 없는 것

- HTTPS와 `CONNECT` 메서드. Forward Proxy는 `http://`만 됩니다.
- IPv6. IPv4만 됩니다.
- 웹 서버의 `HEAD`, `PUT`, `DELETE` 등 `GET`·`POST` 밖의 메서드.
- 동시에 만드는 자식 프로세스 수와 스레드 수의 상한.
- 프록시의 `X-Forwarded-For` 헤더, chunked 응답 처리.
- 같은 응답이 캐시에 없는 순간에 요청이 몰리면 백엔드가 그만큼 일합니다(캐시 스탬피드).

## 더 보기

| 더 보기 | 문서 |
| --- | --- |
| 과제 전체 그림과 로드맵 | [docs/design.md](docs/design.md) |
| 소켓, 요청 줄, 헤더 | [단계 1](docs/step1.md), [단계 2](docs/step2.md), [단계 3](docs/step3.md) |
| 정적 파일, POST 본문, CGI | [단계 4·5](docs/step4-5.md), [단계 6·7](docs/step6-7.md), [단계 8](docs/step8.md) |
| 동시 연결과 안전 종료 | [단계 9·10](docs/step9-10.md) |
| 프록시, 스레드, keep-alive | [단계 11~13](docs/step11-13.md), [단계 14](docs/step14.md), [단계 15](docs/step15.md) |
| 헬스 체크, 캐시, 연결 보관함 | [단계 16](docs/step16.md), [단계 17](docs/step17.md), [단계 18](docs/step18.md), [단계 19](docs/step19.md) |
| 대기열·블로킹 시연 도구 | [demo/README.md](demo/README.md) |
