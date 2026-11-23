/*
 * 웹 서버 진입점.
 * 단계 4 조각 1: TCP 소켓을 만들고 8080번 포트에 bind → listen (여기까지 한 번)
 *         → [연결 하나를 accept → 헤더의 끝(빈 줄)까지 read를 반복해 요청을 받고 → 요청 줄을 세 칸으로 나눠 출력
 *         → 고정 응답(200 OK, <h1>Hello</h1>)을 보내고 → 연결을 닫는다]를 끝없이 반복한다.
 *         헤더가 버퍼보다 길면 431 에러 응답을 보낸다. 한 번에 연결 하나만 처리한다.
 * 요청 내용에 따라 응답을 바꾸는 것은 아직 없다.
 */
#include "http.h"
#include "net.h"

#include <stddef.h>     /* size_t */
#include <stdio.h>      /* printf, perror, setvbuf */
#include <sys/socket.h> /* SOMAXCONN */
#include <unistd.h>     /* close, write, ssize_t */

#define PORT 8080
#define BACKLOG SOMAXCONN /* 연결 대기열 최대 길이. SOMAXCONN은 <sys/socket.h>에 정의된 시스템 상한 값 */
#define REQ_BUF_SIZE 8192 /* 요청 헤더를 받는 버퍼 크기(바이트). 마지막 한 바이트는 '\0' 자리로 남긴다 */

/*
 * 고정 응답 (HTTP 응답 메시지 = 상태 줄 + 헤더 줄들 + 빈 줄 + 본문).
 * C는 나란히 적은 문자열 상수를 하나로 이어 붙인다.
 *   상태 줄        : HTTP 버전, 상태 코드 200, 설명 문구 OK
 *   Content-Type   : 본문의 종류. text/html은 HTML 문서
 *   Content-Length : 본문의 바이트 수. "<h1>Hello</h1>\n"은 15바이트
 *   Connection     : close는 "이 응답을 보낸 뒤 연결을 닫는다"는 뜻. 연결을 계속 쓰는 기능이 없으므로 넣는다
 *   빈 줄          : 헤더의 끝
 */
#define RESPONSE                    \
    "HTTP/1.1 200 OK\r\n"           \
    "Content-Type: text/html\r\n"   \
    "Content-Length: 15\r\n"        \
    "Connection: close\r\n"         \
    "\r\n"                          \
    "<h1>Hello</h1>\n"

/*
 * 헤더가 버퍼(REQ_BUF_SIZE)보다 길 때 보내는 에러 응답.
 * 431 Request Header Fields Too Large: 요청의 헤더가 너무 커서 서버가 처리하지 않겠다는 상태 코드 (RFC 6585 5절).
 * 본문 "<h1>431 Request Header Fields Too Large</h1>\n"은 45바이트
 */
#define RESPONSE_431                                   \
    "HTTP/1.1 431 Request Header Fields Too Large\r\n" \
    "Content-Type: text/html\r\n"                      \
    "Content-Length: 45\r\n"                           \
    "Connection: close\r\n"                            \
    "\r\n"                                             \
    "<h1>431 Request Header Fields Too Large</h1>\n"

int main(void)
{
    /*
     * 표준 출력을 줄 단위로 내보내게 한다(_IOLBF: 줄바꿈 문자를 만나면 바로 내보냄).
     * 출력이 터미널이 아니라 파일이면 C 라이브러리는 출력을 모아 두었다가 한꺼번에 내보낸다.
     * 이 프로세스는 Ctrl+C로 끝나기 때문에, 모아 둔 출력이 나가지 못한 채 사라지지 않게 한다.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 소켓을 만든다 */
    int fd = create_tcp_socket();
    if (fd == -1) {
        return 1;
    }
    /* fd 번호를 출력한다. */
    printf("socket fd = %d\n", fd);

    /* 서버를 끄고 바로 다시 켜도 bind가 되도록 주소 재사용 허용 옵션(SO_REUSEADDR)을 켠다. bind보다 먼저 해야 한다.
       우리 서버는 연결을 받자마자 close(conn_fd)로 먼저 닫는다. TCP에서는 먼저 닫은 쪽의 커널이 그 연결 기록을 약 60초
       동안 TIME_WAIT 상태로 남겨 둔다. 그 기록에 8080 포트가 들어 있어서 그동안 서버를 다시 실행하면 bind가 이 주소는
       아직 쓰는 중이라고 보고 EADDRINUSE로 실패한다. */
    if (set_reuseaddr(fd) == -1) {
        close(fd);
        return 1;
    }

    /* 소켓에 모든 IPv4 주소의 PORT(8080번) 포트를 붙인다. 실패해도 fd는 닫고 끝낸다 */
    if (bind_port(fd, PORT) == -1) {
        close(fd);
        return 1;
    }
    printf("bind 성공: 0.0.0.0:%d\n", PORT);

    /* 소켓을 듣기 소켓으로 바꾼다. 이때부터 커널이 8080번 포트로 오는 연결 요청을 받아 대기열에 넣는다 */
    if (listen_port(fd, BACKLOG) == -1) {
        close(fd);
        return 1;
    }
    printf("listen 중: 연결을 기다립니다 (다른 터미널에서 curl localhost:%d, 끝내려면 Ctrl+C)\n", PORT);

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

        /*
         * 헤더의 끝(빈 줄, "\r\n\r\n")까지 요청을 읽는다.
         * read_request_head는 "\r\n\r\n"이 나올 때까지 read를 반복해 buf에 이어 붙이고, 끝에 '\0'을 넣어 준다.
         * len에는 받은 바이트 수가 들어온다.
         */
        char buf[REQ_BUF_SIZE];
        size_t len = 0;
        int head = read_request_head(conn_fd, buf, sizeof(buf), &len);
        printf("받은 바이트 수: %zu\n", len);

        if (head == HEAD_ERROR) {
            /* 이 연결에서 read가 실패했다. 이 연결만 닫고 다음 연결을 기다린다 */
            close(conn_fd);
            continue;
        }
        if (head == HEAD_CLOSED) {
            /* 헤더가 끝나기 전에 클라이언트가 연결을 닫았다. 응답을 받을 상대가 없으므로 보내지 않는다 */
            printf("헤더가 끝나기 전에 연결이 닫혔습니다\n");
        } else if (head == HEAD_TOO_LARGE) {
            /* 헤더가 버퍼보다 길다. 요청을 해석하지 않고 431 에러 응답을 보낸다 */
            printf("헤더가 너무 깁니다 (버퍼 %d바이트)\n", REQ_BUF_SIZE);
            ssize_t sent = write(conn_fd, RESPONSE_431, sizeof(RESPONSE_431) - 1);
            if (sent == -1) {
                perror("write");
                close(conn_fd);
                continue;
            }
            printf("431 응답을 보냈습니다 (%zd바이트)\n", sent);
        } else {
            /*
             * 요청 줄을 메서드, 요청 대상, 버전 세 칸으로 나눠 출력한다 (단계 2의 파서).
             * 성공하면 세 칸은 malloc으로 할당된 메모리에 들어 있으므로, 다 쓴 뒤 free_request로 해제한다.
             */
            struct http_request req;
            if (parse_request_line(buf, &req) == 0) {
                printf("메서드: %s\n", req.method);
                printf("요청 대상: %s\n", req.target);
                printf("HTTP 버전: %s\n", req.version);
                free_request(&req);
            } else {
                printf("요청 줄 형식이 맞지 않습니다\n");
            }

            /*
             * 고정 응답을 보낸다. 지금은 요청 내용과 상관없이 항상 같은 응답이다.
             * sizeof(RESPONSE)는 끝의 '\0'까지 센 크기라서 1을 뺀다. '\0'은 HTTP 메시지의 일부가 아니다.
             * write(fd, 시작 주소, 바이트 수)는 프로세스의 바이트를 커널의 송신 버퍼로 복사하고, 복사한 바이트 수를 돌려준다.
             */
            ssize_t sent = write(conn_fd, RESPONSE, sizeof(RESPONSE) - 1);
            if (sent == -1) {
                perror("write");
                close(conn_fd);
                continue;
            }
            printf("응답을 보냈습니다 (%zd바이트)\n", sent);
        }

        /*
         * 연결 소켓을 닫는다. 커널이 연결을 끝내고(FIN), fd 테이블에서 conn_fd 칸이 비워진다.
         * 듣기 소켓(fd)은 그대로 열려 있다. 다음 accept는 비워진 칸의 번호를 다시 쓰므로 conn_fd 번호가 매번 같다.
         * 응답에 Connection: close를 넣었으므로, 클라이언트 프로세스는 이 응답 뒤에 연결이 닫힐 것을 알고 있다.
         */
        if (close(conn_fd) == -1) {
            perror("close(conn_fd)");
            continue;
        }
        printf("연결을 닫았습니다 (conn fd = %d)\n", conn_fd);
    }
}
