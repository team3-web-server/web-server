/*
 * 단계 1 서버: <port>에서 연결 하나를 받아 상대 주소를 출력하고 닫은 뒤 끝난다.
 * 사용법: ./build/server <port>
 */
/* -std=c11은 getnameinfo 같은 POSIX 선언을 숨기므로 켠다 */
#define _POSIX_C_SOURCE 200112L

#include "net.h"

#include <netdb.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static void print_peer(const struct sockaddr *addr, socklen_t len, int connfd)
{
    char host[1025], port[32];
    /* 숫자로만 바꾼다: 이름 조회(DNS)는 느릴 수 있다 */
    int rc = getnameinfo(addr, len, host, sizeof(host), port, sizeof(port),
                         NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        fprintf(stderr, "getnameinfo: %s\n", gai_strerror(rc));
        return;
    }
    printf("accepted (%s, %s) connfd = %d\n", host, port, connfd);
}

static int close_fd(int fd, const char *name)
{
    if (close(fd) == -1) {
        perror(name);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || parse_port(argv[1]) == -1) {
        fprintf(stderr, "usage: %s <port>   (port: 1~65535)\n", argv[0]);
        return 1;
    }

    int listenfd = open_listenfd(argv[1]);
    if (listenfd == -1) {
        return 1;
    }
    printf("listening on port %s (listenfd = %d)\n", argv[1], listenfd);
    fflush(stdout); /* 파이프로 출력할 때도 accept에서 멈추기 전에 보이도록 */

    struct sockaddr_storage clientaddr;
    socklen_t clientlen = sizeof(clientaddr);
    int connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
    if (connfd == -1) {
        perror("accept");
        close(listenfd);
        return 1;
    }
    print_peer((struct sockaddr *)&clientaddr, clientlen, connfd);

    int status = close_fd(connfd, "close connfd");
    status |= close_fd(listenfd, "close listenfd");
    return status;
}
