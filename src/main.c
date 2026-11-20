/*
 * 웹 서버 진입점.
 * 단계 1-3: TCP 소켓을 만들고 8080번 포트에 bind → listen → 연결 하나를 accept → 닫고 종료한다.
 * 요청을 읽거나 응답을 보내지는 않는다. 연결을 여러 번 받는 반복도 아직 없다.
 */
#include "net.h"

#include <stdio.h>      /* printf, perror, fflush */
#include <sys/socket.h> /* SOMAXCONN */
#include <unistd.h>     /* close */

#define PORT 8080
#define BACKLOG SOMAXCONN /* 연결 대기열 최대 길이. SOMAXCONN은 <sys/socket.h>에 정의된 시스템 상한 값 */

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
     * 연결 소켓을 닫는다. 커널이 연결을 끝내고, 듣기 소켓(fd)은 아직 열려 있다.
     * 클라이언트가 보낸 데이터를 읽지 않고 닫으면 커널은 FIN 대신 RST(강제 종료)를 보낸다.
     * 그래서 지금 curl로 접속하면 "Connection reset by peer"가 나온다. 요청을 읽는 것은 다음 단계에서 한다.
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
