/*
 * 웹 서버 진입점.
 * 단계 1-2: TCP 소켓을 만들고 8080번 포트에 bind한 뒤, Enter를 누를 때까지 기다렸다가 닫는다.
 * listen/accept는 다음 조각에서 추가한다.
 */
#include "net.h"

#include <stdio.h>  /* printf, perror, getchar */
#include <unistd.h> /* close */

#define PORT 8080

int main(void)
{
    /* 소켓을 만든다 */
    int fd = create_tcp_socket();
    if (fd == -1) {
        return 1;
    }
    /* fd 번호를 출력한다. */
    printf("socket fd = %d\n", fd);

    /* 소켓에 모든 IPv4 주소의 PORT(8080번) 포트를 붙인다. 실패해도 fd는 닫고 끝낸다 */
    if (bind_port(fd, PORT) == -1) {
        close(fd);
        return 1;
    }
    printf("bind 성공: 0.0.0.0:%d\n", PORT);

    /*
     * [임시] bind한 상태를 유지해서, 다른 터미널에서 한 번 더 실행하면
     * "bind: Address already in use"(EADDRINUSE)가 나는 것을 볼 수 있게 한다.
     * listen/accept가 들어오는 다음 조각에서 지운다.
     * 입력이 없으면(make test처럼 stdin이 비어 있으면) getchar()가 EOF를 돌려줘 바로 넘어간다.
     */
    printf("Enter를 누르면 소켓을 닫고 종료합니다...\n");
    fflush(stdout);
    getchar();

    /* fd를 닫아야 커널이 소켓 객체를 해제하고 fd 테이블의 칸이 비워진다 (포트도 풀린다) */
    if (close(fd) == -1) {
        perror("close");
        return 1;
    }
    return 0;
}
