/*
 * 웹 서버 진입점.
 * 실행: ./build/server [포트] [정적 파일 폴더]     예) ./build/server 8080 ./www
 *       인자를 생략하면 포트 8080, 폴더 ./www를 쓴다.
 * 흐름: 명령행 인자를 읽는다 → TCP 소켓을 만들고 포트에 bind → listen (여기까지 한 번)
 *       → [연결 하나를 accept → handle_connection이 요청을 읽고 응답을 보냄 → 연결을 닫는다]를 끝없이 반복한다.
 * 한 번에 연결 하나만 처리한다.
 */
#include "handler.h"
#include "net.h"

#include <errno.h>      /* errno */
#include <stdio.h>      /* printf, fprintf, perror, setvbuf */
#include <stdlib.h>     /* strtol */
#include <sys/socket.h> /* SOMAXCONN */
#include <unistd.h>     /* close */

#define DEFAULT_PORT 8080
#define DEFAULT_ROOT "./www" /* 정적 파일을 두는 폴더. 요청 대상을 이 폴더 아래의 파일 경로로 바꾼다 */
#define BACKLOG SOMAXCONN    /* 연결 대기열 최대 길이. SOMAXCONN은 <sys/socket.h>에 정의된 시스템 상한 값 */

/*
 * 문자열 text를 포트 번호로 바꾼다. 1~65535 범위의 숫자가 아니면 -1을 돌려준다.
 * strtol(문자열, &end, 10): 문자열 앞부분의 10진수를 long으로 바꾸고, 숫자가 끝난 위치를 end에 넣어 준다.
 * 숫자가 하나도 없으면 end가 text와 같고, 숫자 뒤에 다른 글자가 남아 있으면 *end가 '\0'이 아니다.
 */
static int parse_port(const char *text)
{
    char *end;
    errno = 0;
    long port = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || port < 1 || port > 65535) {
        return -1;
    }
    return (int)port;
}

/*
 * main(argc, argv): 셸이 명령 줄을 공백으로 나눠 넘겨준다.
 *   argc는 칸의 개수, argv는 각 칸의 문자열이다. argv[0]은 프로그램 이름이다.
 *   예) ./build/server 8080 ./www → argc = 3, argv[1] = "8080", argv[2] = "./www"
 */
int main(int argc, char *argv[])
{
    /*
     * 표준 출력을 줄 단위로 내보내게 한다(_IOLBF: 줄바꿈 문자를 만나면 바로 내보냄).
     * 출력이 터미널이 아니라 파일이면 C 라이브러리는 출력을 모아 두었다가 한꺼번에 내보낸다.
     * 이 프로세스는 Ctrl+C로 끝나기 때문에, 모아 둔 출력이 나가지 못한 채 사라지지 않게 한다.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 명령행 인자: 첫째는 포트, 둘째는 정적 파일 폴더. 없으면 기본값을 쓴다 */
    int port = DEFAULT_PORT;
    const char *root = DEFAULT_ROOT;
    if (argc > 3) {
        fprintf(stderr, "사용법: %s [포트] [정적 파일 폴더]\n", argv[0]);
        return 1;
    }
    if (argc >= 2) {
        port = parse_port(argv[1]);
        if (port == -1) {
            fprintf(stderr, "포트는 1~65535 사이의 숫자여야 합니다: %s\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3) {
        root = argv[2];
    }

    /* 소켓을 만든다 */
    int fd = create_tcp_socket();
    if (fd == -1) {
        return 1;
    }
    printf("socket fd = %d\n", fd);

    /*
     * 서버를 끄고 바로 다시 켜도 bind가 되도록 주소 재사용 허용 옵션(SO_REUSEADDR)을 켠다. bind보다 먼저 해야 한다.
     * 웹 서버 프로세스가 연결을 먼저 닫으면 커널이 그 연결 기록을 약 60초 동안 TIME_WAIT 상태로 남겨 둔다.
     * 그동안 서버를 다시 실행하면 bind가 EADDRINUSE로 실패하는데, 이 옵션을 켜면 그 경우에도 bind가 된다.
     */
    if (set_reuseaddr(fd) == -1) {
        close(fd);
        return 1;
    }

    /* 소켓에 모든 IPv4 주소의 port번 포트를 붙인다 */
    if (bind_port(fd, port) == -1) {
        close(fd);
        return 1;
    }
    printf("bind 성공: 0.0.0.0:%d\n", port);

    /* 소켓을 듣기 소켓으로 바꾼다. 이때부터 커널이 이 포트로 오는 연결 요청을 받아 대기열에 넣는다 */
    if (listen_port(fd, BACKLOG) == -1) {
        close(fd);
        return 1;
    }
    printf("listen 중: 포트 %d, 정적 파일 폴더 %s (다른 터미널에서 curl localhost:%d, 끝내려면 Ctrl+C)\n", port, root,
           port);

    /*
     * 여기부터는 연결 하나를 처리하는 일(accept → 요청 읽기 → 응답 쓰기 → close)을 끝없이 반복한다.
     * 위의 socket, bind, listen은 프로세스가 시작할 때 한 번만 한다. 듣기 소켓 fd는 반복하는 동안 계속 열려 있다.
     * 반복을 빠져나오는 코드는 없다. 웹 서버 프로세스는 터미널에서 Ctrl+C를 누르면 끝난다.
     */
    for (;;) {
        /* 연결 하나를 받는다. 연결이 올 때까지 여기서 멈춰 있는다. conn_fd는 그 연결 전용의 새 fd */
        int conn_fd = accept_client(fd);
        if (conn_fd == -1) {
            continue; /* 이번 연결을 받지 못했다. 프로세스를 끝내지 않고 다음 연결을 기다린다 */
        }

        /* 요청을 읽고 응답을 보낸다 (src/handler.c) */
        handle_connection(conn_fd, root);

        /*
         * 연결 소켓을 닫는다. 커널이 연결을 끝내고(FIN), fd 테이블에서 conn_fd 칸이 비워진다.
         * 듣기 소켓(fd)은 그대로 열려 있다. 다음 accept는 비워진 칸의 번호를 다시 쓰므로 conn_fd 번호가 매번 같다.
         * 응답에 Connection: close를 넣었으므로, 클라이언트 프로세스는 이 응답 뒤에 연결이 닫힐 것을 알고 있다.
         */
        if (close(conn_fd) == -1) {
            perror("close");
            continue;
        }
        printf("연결을 닫았습니다 (conn fd = %d)\n", conn_fd);
    }
}
