# junho/: 준호의 작업 폴더

팀원마다 폴더를 나눠 따로 개발한다. 루트의 `src/`, `include/`, `tests/`, `Makefile`은 소망의 코드이고, 준호의 코드는 모두 이 폴더 안에 있다.

| 위치 | 내용 | 명령 (이 폴더 기준) |
|---|---|---|
| `src/`, `include/`, `tests/`, `Makefile` | 팀 로드맵 단계별 서버. 지금은 단계 1(`open_listenfd`로 듣기 소켓을 열고 연결 하나를 `accept`한 뒤 닫기) | `make`, `make test`, `make asan`, `./build/server 8080` |
| `docs/step1.md` | 단계 1의 결정사항·이유·개념·확인 결과 | - |
| `tiny/` | CS:APP 11.4~11.6 책 예제(Tiny 웹 서버, CGI adder, 에코, hostinfo). 자세한 내용은 `tiny/README.md` | `make -C tiny`, `make -C tiny test` |

빌드 결과물(`build/`, `tiny/`의 실행 파일)은 git에 올리지 않는다.
