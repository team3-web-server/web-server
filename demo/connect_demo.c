/*
 * listen_demo의 상대 역할을 하는 시연용 클라이언트.
 * 사용법: ./build/connect_demo <횟수>
 *   127.0.0.1:9090 으로 연결을 <횟수>번 차례로 걸고, 끊지 않고 모두 유지한다.
 *   각 connect()가 돌아오기까지 걸린 시간을 출력한다.
 *   서버 대기열이 가득 차면 connect()가 그 줄에서 멈추고, 자리가 나면 그때 걸린 시간이 찍힌다.
 * 컴파일: 레포 루트에서 make demo (build/connect_demo 가 생긴다)
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, CLOCK_MONOTONIC 을 쓰기 위한 선언 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEMO_PORT 9090

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char *argv[])
{
    int count = (argc >= 2) ? atoi(argv[1]) : 5;

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(DEMO_PORT);
    server.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 */

    for (int i = 1; i <= count; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) { perror("socket"); return 1; }

        printf("연결 %d: connect() 호출...", i);
        fflush(stdout);
        double start = now();
        int ret = connect(fd, (struct sockaddr *)&server, sizeof(server));
        double elapsed = now() - start;
        if (ret == 0) {
            printf(" 성공 (%.2f초 걸림, fd = %d)\n", elapsed, fd);
        } else {
            printf(" 실패 (%.2f초 걸림): ", elapsed);
            fflush(stdout);
            perror("connect");
        }
        /* fd를 닫지 않는다: 연결을 유지해서 서버 대기열에 계속 남아 있게 한다 */
    }

    printf("연결을 모두 유지하는 중. Enter를 누르면 끝냅니다...\n");
    getchar();
    return 0;
}
