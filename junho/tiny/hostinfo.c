/*
 * 도메인 이름에 대응하는 IPv4 주소를 모두 출력한다 (CS:APP 그림 11.17)
 * 사용법: ./hostinfo <domain name>
 */
#include "netlib.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <domain name>\n", argv[0]);
        exit(0);
    }

    struct addrinfo hints, *list, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(argv[1], NULL, &hints, &list);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(rc));
        exit(1);
    }

    for (p = list; p != NULL; p = p->ai_next) {
        char addr[MAXLINE];
        Getnameinfo(p->ai_addr, p->ai_addrlen, addr, MAXLINE, NULL, 0, NI_NUMERICHOST);
        printf("%s\n", addr);
    }

    Freeaddrinfo(list);
    exit(0);
}
