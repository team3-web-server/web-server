/*
 * net.c 단위 테스트. 외부 테스트 라이브러리 없이 표준 C로만 작성한다.
 * 각 테스트는 실패하면 FAIL 줄을 출력하고, 실패가 하나라도 있으면 종료 코드 1.
 */
/* getaddrinfo, getsockname 선언을 켠다 (-std=c11은 POSIX 함수 선언을 숨김) */
#define _POSIX_C_SOURCE 200112L

#include "net.h"

#include <netdb.h>      /* getaddrinfo, getnameinfo */
#include <stdio.h>
#include <string.h>     /* memset */
#include <sys/socket.h> /* accept, connect, getsockname */
#include <unistd.h>     /* close */

static int failures = 0;

#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (cond) {                                        \
            printf("PASS: %s\n", msg);                     \
        } else {                                           \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                    \
        }                                                  \
    } while (0)

static void test_create_tcp_socket(void)
{
    int fd = create_tcp_socket();
    CHECK(fd >= 0, "create_tcp_socket()이 0 이상의 fd를 반환");
    if (fd >= 0) {
        CHECK(close(fd) == 0, "반환된 fd를 close()하면 0을 반환");
    }
}

static void test_parse_port(void)
{
    CHECK(parse_port("8080") == 8080, "parse_port(\"8080\") == 8080");
    CHECK(parse_port("1") == 1, "parse_port(\"1\") == 1 (최솟값)");
    CHECK(parse_port("65535") == 65535, "parse_port(\"65535\") == 65535 (최댓값)");
    CHECK(parse_port("0") == -1, "parse_port(\"0\") == -1");
    CHECK(parse_port("65536") == -1, "parse_port(\"65536\") == -1 (16비트 초과)");
    CHECK(parse_port("-1") == -1, "parse_port(\"-1\") == -1");
    CHECK(parse_port("80x") == -1, "parse_port(\"80x\") == -1 (숫자 뒤 글자)");
    CHECK(parse_port("abc") == -1, "parse_port(\"abc\") == -1");
    CHECK(parse_port("") == -1, "parse_port(\"\") == -1");
    CHECK(parse_port(" 80") == -1, "parse_port(\" 80\") == -1 (앞 공백)");
    CHECK(parse_port("+80") == -1, "parse_port(\"+80\") == -1 (부호)");
    CHECK(parse_port(NULL) == -1, "parse_port(NULL) == -1");
    CHECK(parse_port("99999999999999999999") == -1, "parse_port(오버플로) == -1");
}

/* listenfd가 받은 포트 번호를 문자열로 알아낸다 (open_listenfd("0")이면 커널이 고른 포트) */
static int listen_port(int listenfd, char *serv, size_t servlen)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(listenfd, (struct sockaddr *)&addr, &len) == -1) {
        return -1;
    }
    return getnameinfo((struct sockaddr *)&addr, len, NULL, 0, serv, servlen, NI_NUMERICSERV) == 0 ? 0 : -1;
}

/* 이 컴퓨터(loopback)의 port로 connect한 fd를 돌려준다. IPv4·IPv6 주소를 차례로 시도 (CS:APP open_clientfd) */
static int connect_local(const char *port)
{
    struct addrinfo hints, *listp, *p;
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(NULL, port, &hints, &listp) != 0) {
        return -1;
    }
    for (p = listp; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) {
            continue;
        }
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(listp);
    return fd;
}

static void test_open_listenfd_accept(void)
{
    int listenfd = open_listenfd("0");
    CHECK(listenfd >= 0, "open_listenfd(\"0\")이 0 이상의 fd를 반환");
    if (listenfd < 0) {
        return;
    }
    char serv[32];
    CHECK(listen_port(listenfd, serv, sizeof(serv)) == 0, "듣기 소켓의 포트를 알아냄");

    /* listen 중이면 accept 전에도 connect가 성공한다: 연결 요청은 커널이 받아 큐에 쌓아 둔다 */
    int clientfd = connect_local(serv);
    CHECK(clientfd >= 0, "듣기 소켓 포트로 connect 성공");
    if (clientfd >= 0) {
        int connfd = accept(listenfd, NULL, NULL);
        CHECK(connfd >= 0, "accept가 연결 fd를 반환");
        CHECK(connfd != listenfd, "연결 fd는 듣기 fd와 다른 번호");
        if (connfd >= 0) {
            close(connfd);
        }
        close(clientfd);
    }
    close(listenfd);
}

static void test_open_listenfd_in_use(void)
{
    int first = open_listenfd("0");
    char serv[32];
    if (first < 0 || listen_port(first, serv, sizeof(serv)) != 0) {
        CHECK(0, "포트 충돌 테스트용 듣기 소켓 준비");
        if (first >= 0) close(first);
        return;
    }
    /* SO_REUSEADDR이 있어도 이미 listen 중인 포트는 다시 bind할 수 없다 (stderr의 "bind: Address already in use"는 정상) */
    int second = open_listenfd(serv);
    CHECK(second == -1, "listen 중인 포트로 open_listenfd하면 -1");
    if (second >= 0) close(second);
    close(first);
}

int main(void)
{
    test_create_tcp_socket();
    test_parse_port();
    test_open_listenfd_accept();
    test_open_listenfd_in_use();
    return failures == 0 ? 0 : 1;
}
