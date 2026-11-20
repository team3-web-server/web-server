/* -std=c11은 getaddrinfo 같은 POSIX 선언을 숨기므로 켠다 */
#define _POSIX_C_SOURCE 200112L

#include "net.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LISTENQ 1024

int create_tcp_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
    }
    return fd;
}

int parse_port(const char *s)
{
    /* strtol은 앞 공백과 '+'도 받아 주므로 첫 글자가 숫자인지 먼저 본다 */
    if (s == NULL || !isdigit((unsigned char)s[0])) {
        return -1;
    }

    char *end;
    errno = 0;
    long port = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || port < 1 || port > 65535) {
        return -1;
    }
    return (int)port;
}

int open_listenfd(const char *port)
{
    struct addrinfo hints, *list, *p;
    int listenfd = -1;
    int reuse = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV;

    int rc = getaddrinfo(NULL, port, &hints, &list);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    /* IPv4·IPv6 주소 후보 중 bind에 성공하는 첫 주소를 쓴다 */
    for (p = list; p != NULL; p = p->ai_next) {
        listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listenfd == -1) {
            continue;
        }
        /* 서버를 재시작해도 TIME_WAIT 연결 때문에 bind가 실패하지 않게 한다 */
        if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
            perror("setsockopt");
            close(listenfd);
            continue;
        }
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        perror("bind");
        close(listenfd);
    }
    freeaddrinfo(list);

    if (p == NULL) {
        fprintf(stderr, "open_listenfd: 포트 %s에 bind할 수 있는 주소가 없음\n", port);
        return -1;
    }
    if (listen(listenfd, LISTENQ) == -1) {
        perror("listen");
        close(listenfd);
        return -1;
    }
    return listenfd;
}
