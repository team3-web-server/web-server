/*
 * 표준 입력의 줄을 서버에 보내고 돌아온 줄을 출력한다 (CS:APP 그림 11.20)
 * 사용법: ./echoclient <host> <port>
 */
#include "netlib.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        exit(0);
    }

    int clientfd = Open_clientfd(argv[1], argv[2]);
    char line[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, clientfd);
    while (Fgets(line, MAXLINE, stdin) != NULL) {
        Rio_writen(clientfd, line, strlen(line));
        Rio_readlineb(&rio, line, MAXLINE);
        Fputs(line, stdout);
    }
    Close(clientfd);
    exit(0);
}
