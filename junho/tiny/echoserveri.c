/*
 * 한 번에 한 클라이언트씩 처리하는 반복 에코 서버 (CS:APP 그림 11.21)
 * 사용법: ./echoserveri <port>
 */
#include "netlib.h"

void echo(int connfd);

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }

    int listenfd = Open_listenfd(argv[1]);
    while (1) {
        struct sockaddr_storage clientaddr; /* IPv4·IPv6 어느 주소든 담을 수 있는 크기 */
        socklen_t clientlen = sizeof(clientaddr);
        char host[MAXLINE], port[MAXLINE];

        int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, host, MAXLINE, port, MAXLINE, 0);
        printf("Connected to (%s, %s)\n", host, port);
        echo(connfd);
        Close(connfd);
    }
}
