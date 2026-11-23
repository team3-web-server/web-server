#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int create_tcp_socket(void)
{
    /* IPv4(AF_INET), TCP(SOCK_STREAM) */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        /* perror가 errno를 바꿀 수 있어서 저장했다가 되돌린다 (아래 함수들도 같다) */
        int saved_errno = errno;
        perror("socket");
        errno = saved_errno;
        return -1;
    }
    return fd;
}

int set_reuseaddr(int fd)
{
    int on = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        int saved_errno = errno;
        perror("setsockopt(SO_REUSEADDR)");
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int bind_port(int fd, int port)
{
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;

    /* 포트와 주소는 네트워크 바이트 순서(빅엔디언)로 넣는다. INADDR_ANY는 0.0.0.0(모든 IPv4 주소) */
    addr.sin_port = htons((unsigned short)port);

    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        int saved_errno = errno;
        perror("bind");
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int listen_port(int fd, int backlog)
{
    if (listen(fd, backlog) == -1) {
        int saved_errno = errno;
        perror("listen");
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int accept_client(int fd)
{
    struct sockaddr_in client;

    /* 값-결과 인자: 넘길 때는 구조체 크기, 돌아올 때는 커널이 실제로 적은 크기 */
    socklen_t len = sizeof(client);

    int conn_fd = accept(fd, (struct sockaddr *)&client, &len);
    if (conn_fd == -1) {
        /* 시그널로 중단된 것은 고장이 아니므로 에러 문구를 출력하지 않는다 */
        if (errno == EINTR) {
            return -1;
        }
        int saved_errno = errno;
        perror("accept");
        errno = saved_errno;
        return -1;
    }

    char ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip)) != NULL) {
        printf("연결 수락: 클라이언트 %s:%d (conn fd = %d)\n", ip, ntohs(client.sin_port), conn_fd);
    }
    return conn_fd;
}

int write_all(int fd, const char *buf, size_t len)
{
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, buf + (len - left), left);
        if (n == -1) {
            if (errno == EINTR) {
                /* 시그널로 중단됐을 뿐이므로 다시 쓴다 */
                continue;
            }
            return -1;
        }
        left -= (size_t)n;
    }
    return 0;
}
