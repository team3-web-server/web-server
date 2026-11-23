/*
 * http.c 단위 테스트. 외부 테스트 라이브러리 없이 표준 C로만 작성한다.
 * 각 테스트는 실패하면 FAIL 줄을 출력하고, 실패가 하나라도 있으면 종료 코드 1.
 * 해제하지 않은 메모리(누수)는 make asan이 잡아 준다.
 */
#include "http.h"

#include <stdio.h>
#include <stdlib.h> /* malloc, free */
#include <string.h> /* strcmp, memset, memcpy, strlen */

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

/* curl localhost:5200/index.html 이 실제로 보낸 87바이트 요청 */
static void test_curl_request(void)
{
    const char *buf = "GET /index.html HTTP/1.1\r\n"
                      "Host: localhost:5200\r\n"
                      "User-Agent: curl/8.5.0\r\n"
                      "Accept: */*\r\n"
                      "\r\n";
    struct http_request req;
    CHECK(strlen(buf) == 87, "테스트 입력이 curl 실측과 같은 87바이트");
    int ret = parse_request_line(buf, &req);
    CHECK(ret == 0, "curl 요청 전체를 넘기면 0을 반환");
    if (ret == 0) {
        CHECK(strcmp(req.method, "GET") == 0, "method가 \"GET\"");
        CHECK(strcmp(req.target, "/index.html") == 0, "target이 \"/index.html\"");
        CHECK(strcmp(req.version, "HTTP/1.1") == 0, "version이 \"HTTP/1.1\" (뒤에 \\r이 붙지 않음)");
    }
    free_request(&req);
}

/* 헤더 없이 요청 줄만 있어도 된다 */
static void test_request_line_only(void)
{
    struct http_request req;
    int ret = parse_request_line("POST /submit HTTP/1.0\r\n", &req);
    CHECK(ret == 0, "요청 줄만 있어도 0을 반환");
    if (ret == 0) {
        CHECK(strcmp(req.method, "POST") == 0 && strcmp(req.target, "/submit") == 0 &&
                  strcmp(req.version, "HTTP/1.0") == 0,
              "POST /submit HTTP/1.0 을 세 칸으로 나눔");
    }
    free_request(&req);

    ret = parse_request_line("GET / HTTP/1.1\r\n\r\n", &req);
    CHECK(ret == 0 && strcmp(req.target, "/") == 0, "target이 \"/\" 한 글자여도 나눔");
    free_request(&req);
}

/* 결과는 buf와 따로 할당된 복사본이어야 한다: buf를 덮어써도 결과가 그대로다 */
static void test_result_is_a_copy(void)
{
    char buf[] = "GET /index.html HTTP/1.1\r\n";
    struct http_request req;
    int ret = parse_request_line(buf, &req);
    memset(buf, 'X', sizeof(buf) - 1); /* 입력 버퍼를 전부 'X'로 덮어쓴다 */
    CHECK(ret == 0 && strcmp(req.method, "GET") == 0 && strcmp(req.target, "/index.html") == 0 &&
              strcmp(req.version, "HTTP/1.1") == 0,
          "입력 버퍼를 덮어써도 결과 세 칸이 그대로");
    free_request(&req);
}

/* 실패한 한 줄을 검사한다: -1을 돌려주고 세 포인터가 모두 NULL이어야 한다 (해제할 것이 없음) */
static void check_fails(const char *buf, const char *msg)
{
    struct http_request req;
    int ret = parse_request_line(buf, &req);
    CHECK(ret == -1 && req.method == NULL && req.target == NULL && req.version == NULL, msg);
    free_request(&req); /* 실패 뒤에 불러도 안전해야 한다 */
}

static void test_bad_request_lines(void)
{
    check_fails("GET /index.html HTTP/1.1", "CRLF가 없으면 -1이고 포인터는 모두 NULL");
    check_fails("GET /index.html HTTP/1.1\n", "줄 끝이 LF만 있으면 -1");
    check_fails("GET/index.html HTTP/1.1\r\n", "공백이 하나뿐이면(칸 두 개) -1");
    check_fails("GET\r\nHost: a b c\r\n", "첫 줄에 공백이 없으면 다음 줄에 공백이 있어도 -1");
    check_fails("GET /a b HTTP/1.1\r\n", "공백이 세 개면(칸 네 개) -1");
    check_fails("GET  HTTP/1.1\r\n", "가운데 칸이 비어 있으면 -1");
    check_fails(" /index.html HTTP/1.1\r\n", "첫 칸이 비어 있으면 -1");
    check_fails("GET /index.html \r\n", "마지막 칸이 비어 있으면 -1");
    check_fails("", "빈 문자열이면 -1");
    check_fails("\r\n", "빈 줄이면 -1");
    check_fails(NULL, "buf가 NULL이면 -1");
    CHECK(parse_request_line("GET / HTTP/1.1\r\n", NULL) == -1, "req가 NULL이면 -1");
}

/* 칸 길이에 고정된 상한이 없다: 긴 요청 대상도 그 길이만큼 할당해서 담는다 */
static void test_long_target(void)
{
    const size_t target_len = 5000;
    const char *tail = " HTTP/1.1\r\n";
    char *buf = malloc(4 + target_len + strlen(tail) + 1);
    if (buf == NULL) {
        CHECK(0, "긴 요청 대상 테스트용 버퍼 할당");
        return;
    }
    memcpy(buf, "GET ", 4);
    buf[4] = '/';
    memset(buf + 5, 'a', target_len - 1); /* '/' 한 글자 + 'a' 4999개 = 5000글자 */
    strcpy(buf + 4 + target_len, tail);

    struct http_request req;
    int ret = parse_request_line(buf, &req);
    CHECK(ret == 0 && strlen(req.target) == target_len, "요청 대상이 5000글자여도 0을 반환하고 길이가 그대로");
    CHECK(ret == 0 && strcmp(req.version, "HTTP/1.1") == 0, "긴 요청 대상 뒤의 version도 맞음");
    free_request(&req);
    free(buf);
}

static void test_free_request(void)
{
    struct http_request req;
    int ret = parse_request_line("GET / HTTP/1.1\r\n", &req);
    free_request(&req);
    CHECK(ret == 0 && req.method == NULL && req.target == NULL && req.version == NULL,
          "free_request 뒤에는 세 포인터가 모두 NULL");
    free_request(&req); /* 두 번 불러도 이중 해제가 일어나지 않아야 한다 (asan이 검사) */
    free_request(NULL); /* NULL을 넘겨도 죽지 않아야 한다 */
    CHECK(1, "free_request를 두 번 부르거나 NULL을 넘겨도 안전");
}

int main(void)
{
    test_curl_request();
    test_request_line_only();
    test_result_is_a_copy();
    test_bad_request_lines();
    test_long_target();
    test_free_request();

    if (failures > 0) {
        printf("단위 테스트 실패: %d개\n", failures);
        return 1;
    }
    printf("단위 테스트 모두 통과\n");
    return 0;
}
