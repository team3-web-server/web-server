/* -std=c11은 POSIX 함수 선언(getaddrinfo 등)을 숨기므로 POSIX.1-2001 기능을 켠다 */
#define _POSIX_C_SOURCE 200112L

#include "net.h"

#include <ctype.h>      /* isdigit */
#include <errno.h>      /* errno */
#include <netdb.h>      /* getaddrinfo, freeaddrinfo, gai_strerror, struct addrinfo */
#include <stdio.h>      /* perror, fprintf */
#include <stdlib.h>     /* strtol */
#include <string.h>     /* memset */
#include <sys/socket.h> /* socket, bind, listen, setsockopt, AF_INET, SOCK_STREAM */
#include <unistd.h>     /* close */

/* listen()의 backlog: 아직 accept하지 않은 연결 요청을 커널이 쌓아 둘 개수의 힌트 (CS:APP의 LISTENQ) */
#define LISTENQ 1024

/* TCP 소켓을 하나 만들어서 그 fd 번호를 돌려주는 함수. 만들지 못하면 에러 원인을 출력하고 -1을 돌려준다. */
int create_tcp_socket(void)
{
    /*
     * AF_INET    : 주소 체계는 IPv4
     * SOCK_STREAM: 연결을 맺고 바이트를 순서대로 전달하는 스트림 소켓 (= TCP)
     * 0          : 위 두 값에 맞는 기본 프로토콜(TCP)을 커널이 고른다
     */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        /* socket()은 실패하면 -1을 반환하고 원인을 전역 변수 errno에 남긴다 */
        perror("socket");
        return -1;
    }
    return fd;
}

int parse_port(const char *s)
{
    /* strtol은 앞 공백과 '+'를 허용하므로 첫 글자가 숫자인지 따로 본다 (" 80", "+80" 거부) */
    if (s == NULL || !isdigit((unsigned char)s[0])) {
        return -1;
    }
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    /* end가 문자열 끝이 아니면 "80x"처럼 숫자 뒤에 다른 글자가 붙은 것 */
    if (errno != 0 || *end != '\0' || v < 1 || v > 65535) {
        return -1;
    }
    return (int)v;
}

int open_listenfd(const char *port)
{
    struct addrinfo hints, *listp, *p;
    int listenfd = -1, optval = 1, rc;

    /* 어떤 주소를 원하는지 hints에 적는다. 나머지 칸은 0이어야 하므로 먼저 전부 0으로 채운다 */
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;             /* 연결(TCP)용 주소만 */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* 서버용 와일드카드 주소, 이 호스트에 설정된 IP 버전만 */
    hints.ai_flags |= AI_NUMERICSERV;            /* port는 서비스 이름("http")이 아니라 숫자 */

    /* getaddrinfo는 errno가 아니라 반환값으로 에러 코드를 주므로 perror 대신 gai_strerror를 쓴다 */
    if ((rc = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    /* 목록(IPv4, IPv6 주소가 섞일 수 있음)을 돌며 bind에 성공하는 주소를 찾는다 */
    for (p = listp; p != NULL; p = p->ai_next) {
        listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listenfd == -1) {
            continue; /* 이 주소 체계로는 소켓을 못 만듦, 다음 주소로 */
        }

        /* 서버를 껐다 바로 켜도 "Address already in use" 없이 같은 포트에 bind할 수 있게 한다 */
        if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
            perror("setsockopt");
            close(listenfd);
            continue;
        }

        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0) {
            break; /* 성공 */
        }
        perror("bind");
        close(listenfd); /* bind 실패: fd가 새지 않도록 닫고 다음 주소로 */
    }

    /* 목록은 getaddrinfo가 malloc한 메모리이므로 성공·실패와 상관없이 해제한다 */
    freeaddrinfo(listp);
    if (p == NULL) {
        fprintf(stderr, "open_listenfd: 포트 %s에 bind할 수 있는 주소가 없음\n", port);
        return -1;
    }

    /* 능동 소켓을 듣기 소켓으로 바꾼다. 이제부터 커널이 이 포트로 오는 연결 요청을 받아 큐에 쌓는다 */
    if (listen(listenfd, LISTENQ) == -1) {
        perror("listen");
        close(listenfd);
        return -1;
    }
    return listenfd;
}
