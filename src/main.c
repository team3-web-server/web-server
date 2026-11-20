/*
 * 웹 서버 진입점.
 * 단계 1: <port>에 듣기 소켓을 열고, 연결 하나를 accept해 상대 주소를 출력한 뒤 닫고 끝난다.
 * 아직 요청 데이터는 읽지 않는다 (단계 2부터).
 */
/* getnameinfo, NI_NUMERICHOST 선언을 켠다 (-std=c11은 POSIX 함수 선언을 숨김) */
#define _POSIX_C_SOURCE 200112L

#include "net.h"

#include <netdb.h>      /* getnameinfo, gai_strerror, NI_NUMERICHOST, NI_NUMERICSERV */
#include <stdio.h>      /* printf, fprintf, perror */
#include <sys/socket.h> /* accept, struct sockaddr_storage, socklen_t */
#include <unistd.h>     /* close */

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
    fflush(stdout); /* 출력이 파이프로 갈 때도 accept에서 멈추기 전에 보이도록 */

    /* sockaddr_storage는 IPv4·IPv6 어느 주소든 담을 만큼 크다 (CS:APP 그림 11.21) */
    struct sockaddr_storage clientaddr;
    socklen_t clientlen = sizeof(clientaddr);

    /* 클라이언트가 connect할 때까지 여기서 멈춘다. 연결되면 그 연결 전용 새 fd(connfd)를 받는다 */
    int connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen);
    if (connfd == -1) {
        perror("accept");
        close(listenfd);
        return 1;
    }

    /* 숫자 형식으로만 바꾼다: 도메인 이름 조회(DNS)를 하면 응답이 늦어질 수 있다 */
    char host[1025], serv[32];
    int rc = getnameinfo((struct sockaddr *)&clientaddr, clientlen, host, sizeof(host),
                         serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        fprintf(stderr, "getnameinfo: %s\n", gai_strerror(rc));
    } else {
        printf("accepted (%s, %s) connfd = %d\n", host, serv, connfd);
    }

    /* 연결 fd를 닫으면 클라이언트는 EOF를 받는다(curl: Empty reply from server). 듣기 fd도 닫고 끝낸다 */
    int status = 0;
    if (close(connfd) == -1) {
        perror("close connfd");
        status = 1;
    }
    if (close(listenfd) == -1) {
        perror("close listenfd");
        status = 1;
    }
    return status;
}
