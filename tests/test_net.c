/*
 * net.c 단위 테스트. 외부 테스트 라이브러리 없이 표준 C로만 작성한다.
 * 각 테스트는 실패하면 FAIL 줄을 출력하고, 실패가 하나라도 있으면 종료 코드 1.
 */
#include "net.h"

#include <errno.h>  /* errno, EADDRINUSE */
#include <stdio.h>
#include <unistd.h> /* close */

/* 테스트용 포트. 서버 기본 포트(8080)가 다른 프로그램에 잡혀 있어도 테스트가 깨지지 않게 따로 둔다 */
#define TEST_PORT 18080

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

int main(void)
{
    test_create_tcp_socket();
    test_bind_port();
    test_bind_port_in_use();
    return failures == 0 ? 0 : 1;
}
