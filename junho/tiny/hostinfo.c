/* hostinfo.c - 도메인 이름에 대응하는 IP 주소들을 출력한다 (CS:APP 그림 11.17) */
#include "csapp.h"

int main(int argc, char **argv)
{
    struct addrinfo *p, *listp, hints;
    char buf[MAXLINE];
    int rc, flags;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <domain name>\n", argv[0]);
        exit(0);
    }

    /* addrinfo 레코드 목록을 받는다 */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;       /* IPv4만 */
    hints.ai_socktype = SOCK_STREAM; /* 연결(connection)용만 */
    if ((rc = getaddrinfo(argv[1], NULL, &hints, &listp)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(rc));
        exit(1);
    }

    /* 목록을 순회하며 IP 주소를 하나씩 출력 */
    flags = NI_NUMERICHOST; /* 도메인 이름 대신 주소 문자열을 출력 */
    for (p = listp; p; p = p->ai_next) {
        Getnameinfo(p->ai_addr, p->ai_addrlen, buf, MAXLINE, NULL, 0, flags);
        printf("%s\n", buf);
    }

    /* 정리 */
    Freeaddrinfo(listp);

    exit(0);
}
