# listen·accept 시연 도구

서버 본체(`src/`)가 아니라, 단계 1-3에서 배운 상황을 눈으로 확인하기 위한 도구다. `make`, `make test`에는 포함되지 않는다.

| 파일 | 하는 일 |
|---|---|
| `listen_demo.c` | 9090번 포트에 `listen(backlog)`한 뒤 Enter를 누를 때까지 `accept`를 부르지 않는다. Enter를 누르면 `accept(fd, NULL, NULL)`을 한 번 부르고, 다시 Enter를 누를 때까지 듣기 소켓과 연결 소켓을 열어 둔다 |
| `connect_demo.c` | `127.0.0.1:9090`으로 연결을 차례로 걸고 끊지 않는다. 각 `connect()`에 걸린 시간을 출력한다 |
| `watch_retrans.sh` | 커널의 SYN 재전송 횟수(`/proc/net/netstat`의 `TCPSynRetrans`)가 늘 때마다 한 줄씩 출력한다 |

빌드: 레포 루트에서 `make demo` → `build/listen_demo`, `build/connect_demo`

실행 순서와 확인할 내용은 `docs/step1.md`의 "1-3 시연" 절에 있다.
