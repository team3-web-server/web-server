/*
 * listen·accept 시연용 프로그램 (서버 본체가 아니라 코드 리뷰 때 상황을 보여 주기 위한 도구).
 * 사용법: ./build/listen_demo <backlog>
 *   1) 9090번 포트에 bind하고 listen(backlog)한 뒤, Enter를 누를 때까지 accept를 부르지 않는다.
 *      → 그동안 들어온 연결은 대기열에 쌓이기만 한다.
 *   2) Enter를 누르면 accept(fd, NULL, NULL)로 연결 하나를 꺼낸다. 대기열이 비어 있으면 여기서 멈춘다.
 *   3) 다시 Enter를 누를 때까지 듣기 소켓과 연결 소켓을 둘 다 열어 둔다.
 * 컴파일: 레포 루트에서 make demo (build/listen_demo 가 생긴다)
 */
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEMO_PORT 9090

int main(int argc, char *argv[])
{
    int backlog = (argc >= 2) ? atoi(argv[1]) : 2;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) { perror("socket"); return 1; }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEMO_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) { perror("bind"); close(fd); return 1; }

    int ret = listen(fd, backlog);
    if (ret == -1) { perror("listen"); close(fd); return 1; }
    printf("pid = %d, 듣기 소켓 fd = %d, 포트 = %d\n", getpid(), fd, DEMO_PORT);
    printf("listen(fd, %d) 반환값 = %d\n", backlog, ret);
    printf("[1] accept를 부르지 않고 기다리는 중. Enter를 누르면 accept를 부릅니다...\n");
    fflush(stdout);
    getchar();

    printf("[2] accept(fd, NULL, NULL) 호출. 대기열이 비어 있으면 여기서 멈춥니다...\n");
    fflush(stdout);
    int conn_fd = accept(fd, NULL, NULL); /* 클라이언트 주소가 필요 없어서 둘 다 NULL */
    if (conn_fd == -1) { perror("accept"); close(fd); return 1; }
    printf("accept 반환: 연결 소켓 fd = %d (듣기 소켓 fd = %d 는 그대로)\n", conn_fd, fd);
    printf("[3] 두 소켓을 열어 둔 채 기다리는 중. Enter를 누르면 둘 다 닫고 끝납니다...\n");
    fflush(stdout);
    getchar();

    close(conn_fd);
    close(fd);
    return 0;
}
