/*
 * net.c 단위 테스트. 외부 테스트 라이브러리 없이 표준 C로만 작성한다.
 * 각 테스트는 실패하면 FAIL 줄을 출력하고, 실패가 하나라도 있으면 종료 코드 1.
 */
#include "net.h"

#include <arpa/inet.h>  /* htonl, htons */
#include <errno.h>      /* errno, EADDRINUSE */
#include <netinet/in.h> /* struct sockaddr_in, INADDR_LOOPBACK */
#include <stdio.h>
#include <stdlib.h>     /* malloc, free */
#include <string.h>     /* memset, memcmp */
#include <sys/socket.h> /* connect, getsockopt, SOMAXCONN */
#include <sys/types.h>  /* pid_t, ssize_t */
#include <sys/wait.h>   /* waitpid, WIFEXITED, WEXITSTATUS */
#include <unistd.h>     /* close, read, fork, _exit */

/* 테스트용 포트. 서버 기본 포트(8080)가 다른 프로그램에 잡혀 있어도 테스트가 깨지지 않게 따로 둔다 */
#define TEST_PORT 18080
/* listen·accept 테스트용 포트. bind 테스트와 겹치지 않게 따로 둔다 */
#define TEST_LISTEN_PORT 18081

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

static void test_bind_port(void)
{
    int fd = create_tcp_socket();
    if (fd < 0) {
        CHECK(0, "bind_port 테스트용 소켓 생성");
        return;
    }
    CHECK(bind_port(fd, TEST_PORT) == 0, "빈 포트에 bind_port()하면 0을 반환");
    close(fd);
}

static void test_bind_port_in_use(void)
{
    int first = create_tcp_socket();
    int second = create_tcp_socket();
    if (first < 0 || second < 0) {
        CHECK(0, "EADDRINUSE 테스트용 소켓 생성");
        if (first >= 0) close(first);
        if (second >= 0) close(second);
        return;
    }
    CHECK(bind_port(first, TEST_PORT) == 0, "첫 번째 소켓은 bind 성공");

    /* 같은 포트를 두 번째 소켓이 요청하면 실패해야 한다 (stderr에 "bind: Address already in use"가 출력되는 것이 정상) */
    int ret = bind_port(second, TEST_PORT);
    int saved = errno; /* 다른 함수가 errno를 덮어쓰기 전에 저장한다 */
    CHECK(ret == -1, "같은 포트에 두 번째 bind_port()는 -1을 반환");
    CHECK(saved == EADDRINUSE, "그때 errno는 EADDRINUSE");

    close(second);
    close(first);
}

static void test_set_reuseaddr(void)
{
    int fd = create_tcp_socket();
    if (fd < 0) {
        CHECK(0, "set_reuseaddr 테스트용 소켓 생성");
        return;
    }
    CHECK(set_reuseaddr(fd) == 0, "set_reuseaddr()가 0을 반환");

    /* getsockopt로 커널에 실제로 옵션이 켜졌는지 되물어 본다 (켜져 있으면 0이 아닌 값) */
    int value = 0;
    socklen_t len = sizeof(value);
    int ret = getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, &len);
    CHECK(ret == 0 && value != 0, "getsockopt로 읽은 SO_REUSEADDR 값이 켜져 있음");
    close(fd);
}

static void test_listen_port(void)
{
    int fd = create_tcp_socket();
    if (fd < 0) {
        CHECK(0, "listen_port 테스트용 소켓 생성");
        return;
    }
    if (bind_port(fd, TEST_LISTEN_PORT) == -1) {
        CHECK(0, "listen_port 테스트용 bind");
        close(fd);
        return;
    }
    CHECK(listen_port(fd, SOMAXCONN) == 0, "bind된 소켓에 listen_port()하면 0을 반환");
    close(fd);
}

static void test_accept_client(void)
{
    /* 서버 쪽: 듣기 소켓을 준비한다 */
    int listen_fd = create_tcp_socket();
    if (listen_fd < 0 || bind_port(listen_fd, TEST_LISTEN_PORT) == -1 || listen_port(listen_fd, SOMAXCONN) == -1) {
        CHECK(0, "accept_client 테스트용 듣기 소켓 준비");
        if (listen_fd >= 0) close(listen_fd);
        return;
    }

    /* 클라이언트 쪽: 같은 프로그램 안에서 소켓을 하나 더 만들어 127.0.0.1:TEST_LISTEN_PORT로 연결을 건다 */
    int client_fd = create_tcp_socket();
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(TEST_LISTEN_PORT);
    server.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 */

    /* 아직 accept를 부르지 않았는데도 connect가 성공한다: handshake는 커널이 끝내고 대기열에 넣어 두기 때문 */
    CHECK(connect(client_fd, (struct sockaddr *)&server, sizeof(server)) == 0,
          "accept 전에도 connect()가 성공 (커널이 handshake 후 대기열에 넣음)");

    /* 대기열에서 그 연결을 꺼낸다 (stdout에 "연결 수락: ..." 줄이 출력되는 것이 정상) */
    int conn_fd = accept_client(listen_fd);
    CHECK(conn_fd >= 0, "accept_client()가 0 이상의 fd를 반환");
    CHECK(conn_fd != listen_fd, "연결 소켓 fd는 듣기 소켓 fd와 다른 번호");

    /* 클라이언트가 먼저 닫으면(FIN) 서버 쪽 연결 소켓의 read()는 0(EOF)을 돌려준다 */
    close(client_fd);
    if (conn_fd >= 0) {
        char buf[1];
        CHECK(read(conn_fd, buf, sizeof(buf)) == 0, "클라이언트가 닫은 뒤 연결 소켓의 read()는 0");
        CHECK(close(conn_fd) == 0, "연결 소켓을 close()하면 0을 반환");
    }
    CHECK(close(listen_fd) == 0, "연결 소켓을 닫은 뒤에도 듣기 소켓은 열려 있어 close()가 0을 반환");
}

/* write_all: 짧은 데이터(고정 응답과 같은 98바이트)를 쓰면 반대쪽에서 같은 바이트가 그대로 읽힌다 */
static void test_write_all_small(void)
{
    const char *msg = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 15\r\nConnection: close\r\n\r\n<h1>Hello</h1>\n";
    size_t len = strlen(msg);
    int fds[2];
    CHECK(len == 98, "고정 응답이 98바이트");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1) {
        perror("socketpair");
        failures++;
        return;
    }
    CHECK(write_all(fds[1], msg, len) == 0, "write_all이 98바이트를 쓰고 0을 반환");
    close(fds[1]);

    char buf[256];
    size_t total = 0;
    ssize_t n;
    while ((n = read(fds[0], buf + total, sizeof(buf) - total)) > 0) {
        total += (size_t)n;
    }
    close(fds[0]);
    CHECK(total == len && memcmp(buf, msg, len) == 0, "반대쪽에서 읽은 98바이트가 보낸 것과 같음");
}

/*
 * write_all: 커널의 송신 버퍼보다 큰 데이터(4MB)를 쓴다.
 * 읽는 쪽(자식 프로세스)이 조금씩 읽어 가는 동안 write는 여러 번에 나뉘어 쓰이게 되고,
 * 그래도 write_all은 전부 쓴 뒤에 0을 돌려줘야 한다. 자식은 받은 바이트 수와 내용을 확인해 종료 코드로 알린다.
 */
static void test_write_all_large(void)
{
    size_t len = 4 * 1024 * 1024;
    char *data = malloc(len);
    if (data == NULL) {
        failures++;
        return;
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = (char)('a' + i % 26);
    }
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1) {
        perror("socketpair");
        failures++;
        free(data);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        /* 자식 프로세스: 끝까지 읽으면서 바이트 수와 내용을 확인한다 */
        close(fds[1]);
        char buf[1000];
        size_t total = 0;
        int ok = 1;
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] != (char)('a' + (total + (size_t)i) % 26)) {
                    ok = 0;
                }
            }
            total += (size_t)n;
        }
        _exit(ok && total == len ? 0 : 1);
    }
    close(fds[0]);
    CHECK(write_all(fds[1], data, len) == 0, "write_all이 4MB를 모두 쓰고 0을 반환");
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "반대쪽에서 4MB를 순서대로 빠짐없이 받음");
    free(data);
}

/* write_all: 닫힌 fd에 쓰면 -1 */
static void test_write_all_bad_fd(void)
{
    CHECK(write_all(-1, "x", 1) == -1, "잘못된 fd에 write_all을 하면 -1을 반환");
    CHECK(errno == EBADF, "그때 errno는 EBADF");
    CHECK(write_all(-1, "x", 0) == 0, "쓸 바이트가 0이면 write를 부르지 않고 0을 반환");
}

int main(void)
{
    test_create_tcp_socket();
    test_bind_port();
    test_bind_port_in_use();
    test_set_reuseaddr();
    test_listen_port();
    test_accept_client();
    test_write_all_small();
    test_write_all_large();
    test_write_all_bad_fd();
    return failures == 0 ? 0 : 1;
}
