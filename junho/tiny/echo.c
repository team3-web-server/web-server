/* 연결에서 읽은 줄을 그대로 돌려보낸다 (CS:APP 그림 11.22) */
#include "netlib.h"

void echo(int connfd)
{
    char line[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);
    while (1) {
        ssize_t len = Rio_readlineb(&rio, line, MAXLINE);
        if (len == 0) {
            break; /* 클라이언트가 연결을 닫음 */
        }
        printf("server received %d bytes\n", (int)len);
        Rio_writen(connfd, line, len);
    }
}
