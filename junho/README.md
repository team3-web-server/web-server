# junho/: 준호의 작업 폴더

팀원마다 폴더를 나눠 따로 개발한다. 루트의 `src/`, `include/`, `tests/`, `Makefile`은 소망의 코드이고, 준호의 코드는 모두 이 폴더 안에 있다.

| 위치 | 내용 | 명령 (이 폴더 기준) |
|---|---|---|
| `src/`, `include/`, `tests/`, `Makefile` | 단계 1 서버와 CS:APP 11장 Tiny HTTP 서버 소스. `src/step1_main.c`·`src/net.c`는 단계 1(`open_listenfd`로 듣기 소켓을 열고 연결 하나를 `accept`한 뒤 닫기), `src/tiny.c`·`src/netlib.c`는 Tiny | `make`(둘 다 빌드), `make test`, `make tiny-test`, `make asan`, `./build/server 8080` |
| `docs/step1.md` | 단계 1의 결정사항·이유·개념·확인 결과 | - |
| `tiny/` | Tiny 실행에 필요한 CGI·에셋과 에코/hostinfo 보조 예제, 테스트. 서버 소스는 `src/`로 옮겼다. 자세한 내용은 `tiny/README.md` | `make -C tiny`, `make -C tiny test` |

빌드 결과물(`build/`, `tiny/`의 실행 파일)은 git에 올리지 않는다.
