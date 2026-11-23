/*
 * 웹 서버 진입점.
 * 단계 2: TCP 소켓을 만들고 8080번 포트에 bind → listen → 연결 하나를 accept
 *         → read를 한 번 불러 요청을 받고 → 요청 줄을 세 칸으로 나눠 출력 → 닫고 종료한다.
 * 응답은 아직 보내지 않는다. 요청이 여러 번에 나뉘어 올 때 반복해서 읽는 것, 연결을 여러 번 받는 반복도 아직 없다.
 */
#include "http.h"
#include "net.h"

#include <stdio.h>      /* printf, perror, fflush */
#include <sys/socket.h> /* SOMAXCONN */
#include <sys/types.h>  /* ssize_t */
#include <unistd.h>     /* read, close */

#define PORT 8080
#define BACKLOG SOMAXCONN /* 연결 대기열 최대 길이. SOMAXCONN은 <sys/socket.h>에 정의된 시스템 상한 값 */
#define REQ_BUF_SIZE 4096 /* 요청을 받는 버퍼 크기(바이트). 마지막 한 바이트는 '\0' 자리로 남긴다 */

int main(void)
{
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
    printf("listen 중: 연결을 기다립니다 (다른 터미널에서 curl localhost:%d)\n", PORT);
    fflush(stdout); /* accept에서 멈추기 전에 지금까지의 출력을 화면에 내보낸다 */

    /* 연결 하나를 받는다. 연결이 올 때까지 여기서 멈춰 있는다. conn_fd는 그 연결 전용의 새 fd */
    int conn_fd = accept_client(fd);
    if (conn_fd == -1) {
        close(fd);
        return 1;
    }

    /*
     * 요청을 읽는다. read(fd, 버퍼, 최대 바이트 수)는 커널의 수신 버퍼에 도착해 있는 바이트를 buf로 복사하고
     * 복사한 바이트 수를 돌려준다. 아직 아무것도 도착하지 않았으면 도착할 때까지 멈춰 있는다.
     *   반환값 > 0: 받은 바이트 수 (요청 전체가 아닐 수도 있다. 지금은 한 번만 읽는다)
     *   반환값 0: 상대가 아무것도 보내지 않고 연결을 닫았다
     *   반환값 -1: 에러
     * 최대 바이트 수를 sizeof(buf) - 1로 줘서 '\0'을 넣을 자리 한 바이트를 남긴다.
     * ssize_t: 바이트 수 또는 -1을 담는 부호 있는 정수 타입
     */
    char buf[REQ_BUF_SIZE];
    ssize_t n = read(conn_fd, buf, sizeof(buf) - 1);
    if (n == -1) {
        perror("read");
        close(conn_fd);
        close(fd);
        return 1;
    }
    if (n == 0) {
        printf("요청 없이 연결이 닫혔습니다\n");
    } else {
        /*
         * read는 받은 바이트만 복사하고 끝에 '\0'을 붙여 주지 않는다.
         * strstr 같은 문자열 함수는 '\0'을 만날 때까지 읽으므로, 받은 바이트 바로 뒤에 직접 '\0'을 넣는다.
         */
        buf[n] = '\0';
        printf("받은 바이트 수: %zd\n", n);

        /*
         * 요청 줄(첫 줄)을 메서드, 요청 대상, 버전 세 칸으로 나눈다.
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
    }

    /*
     * 연결 소켓을 닫는다. 커널이 연결을 끝내고, 듣기 소켓(fd)은 아직 열려 있다.
     * 응답을 보내지 않고 닫으므로 curl에는 "Empty reply from server"가 나온다.
     * (받은 데이터를 다 읽은 뒤 닫았기 때문에 커널은 RST가 아니라 FIN을 보낸다.) 응답을 보내는 것은 다음 단계에서 한다.
     */
    if (close(conn_fd) == -1) {
        perror("close(conn_fd)");
        close(fd);
        return 1;
    }
    printf("연결을 닫았습니다 (conn fd = %d)\n", conn_fd);

    /* 듣기 소켓을 닫는다. 커널이 소켓 객체를 해제하고 fd 테이블의 칸이 비워진다 (포트도 풀린다) */
    if (close(fd) == -1) {
        perror("close");
        return 1;
    }
    return 0;
}
