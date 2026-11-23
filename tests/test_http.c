/*
 * http.c 단위 테스트. 외부 테스트 라이브러리 없이 표준 C로만 작성한다.
 * 각 테스트는 실패하면 FAIL 줄을 출력하고, 실패가 하나라도 있으면 종료 코드 1.
 * 해제하지 않은 메모리(누수)는 make asan이 잡아 준다.
 */
/* usleep을 쓰려면 <unistd.h>보다 먼저 이 매크로가 있어야 한다 (-std=c11에서는 기본으로 꺼져 있다) */
#define _DEFAULT_SOURCE

#include "http.h"

#include <stdio.h>
#include <stdlib.h> /* malloc, free */
#include <string.h> /* strcmp, memset, memcpy, strlen */
#include <sys/socket.h> /* socketpair, AF_UNIX, SOCK_STREAM */
#include <sys/types.h>  /* pid_t, ssize_t */
#include <sys/wait.h>   /* waitpid */
#include <unistd.h>     /* fork, write, close, usleep, _exit */

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

/*
 * read_request_head 테스트 도우미.
 * socketpair로 서로 연결된 소켓 두 개를 만든다. fds[0]은 서버 역할(읽는 쪽), fds[1]은 클라이언트 역할(쓰는 쪽).
 * fork로 만든 자식 프로세스가 클라이언트 역할을 맡아 pieces의 조각들을 0.05초 간격으로 하나씩 보낸 뒤 닫는다.
 * 조각 사이에 간격을 두는 이유: 읽는 쪽의 read가 조각마다 따로 돌아오게 해서 "요청이 나뉘어 오는 상황"을 만든다.
 * 부모 프로세스는 fds[0]에서 read_request_head를 부르고 그 반환값을 돌려준다.
 */
static int run_head(const char *const pieces[], int count, char *buf, size_t size, size_t *len)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1) {
        perror("socketpair");
        return -100;
    }
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(fds[0]);
        close(fds[1]);
        return -100;
    }
    if (pid == 0) {
        /* 자식 프로세스: 클라이언트 역할 */
        close(fds[0]);
        for (int i = 0; i < count; i++) {
            size_t left = strlen(pieces[i]);
            const char *p = pieces[i];
            while (left > 0) {
                ssize_t n = write(fds[1], p, left);
                if (n <= 0) {
                    _exit(0); /* 읽는 쪽이 먼저 닫았다 */
                }
                p += n;
                left -= (size_t)n;
            }
            usleep(50000);
        }
        close(fds[1]);
        _exit(0);
    }
    /* 부모 프로세스: 서버 역할 */
    close(fds[1]);
    int ret = read_request_head(fds[0], buf, size, len);
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return ret;
}

static void test_head_one_piece(void)
{
    const char *const pieces[] = {"GET / HTTP/1.1\r\nHost: localhost:8080\r\n\r\n"};
    char buf[256];
    size_t len = 0;
    int ret = run_head(pieces, 1, buf, sizeof(buf), &len);
    CHECK(ret == HEAD_OK, "요청이 한 번에 오면 HEAD_OK");
    CHECK(len == 40, "받은 바이트 수가 40");
    CHECK(strlen(buf) == 40, "buf가 '\\0'으로 끝남 (strlen이 40)");
}

static void test_head_split(void)
{
    /* "\r\n\r\n"이 두 조각 사이에서 잘린다: 앞 조각은 "\r\n\r"로 끝나고 뒤 조각은 "\n"뿐이다 */
    const char *const pieces[] = {"GET /index.html HT", "TP/1.1\r\nHost: localhost:8080\r\n\r", "\n"};
    char buf[256];
    size_t len = 0;
    int ret = run_head(pieces, 3, buf, sizeof(buf), &len);
    CHECK(ret == HEAD_OK, "요청이 세 조각으로 나뉘어 와도 HEAD_OK (\\r\\n\\r\\n이 조각 사이에서 잘림)");
    CHECK(len == 50, "세 조각을 이어 붙인 바이트 수가 50");
    CHECK(strcmp(buf, "GET /index.html HTTP/1.1\r\nHost: localhost:8080\r\n\r\n") == 0, "이어 붙인 내용이 원래 요청과 같음");

    struct http_request req;
    int parsed = parse_request_line(buf, &req);
    CHECK(parsed == 0, "이어 붙인 buf를 단계 2 파서에 넘기면 0을 반환");
    if (parsed == 0) {
        CHECK(strcmp(req.target, "/index.html") == 0, "조각 사이에서 잘렸던 요청 줄의 target이 \"/index.html\"");
        free_request(&req);
    }
}

static void test_head_closed(void)
{
    const char *const pieces[] = {"GET / HTTP/1.1\r\nHost: loc"};
    char buf[256];
    size_t len = 0;
    int ret = run_head(pieces, 1, buf, sizeof(buf), &len);
    CHECK(ret == HEAD_CLOSED, "헤더가 끝나기 전에 닫히면 HEAD_CLOSED");
    CHECK(len == 25, "닫히기 전까지 받은 바이트 수가 25");

    ret = run_head(pieces, 0, buf, sizeof(buf), &len);
    CHECK(ret == HEAD_CLOSED, "아무것도 보내지 않고 닫히면 HEAD_CLOSED");
    CHECK(len == 0 && buf[0] == '\0', "받은 바이트 수가 0이고 buf는 빈 문자열");
}

static void test_head_too_large(void)
{
    /* 버퍼 16바이트(받을 수 있는 것은 15바이트)에 그보다 긴 헤더를 보낸다 */
    const char *const pieces[] = {"GET /a-very-long-target HTTP/1.1\r\nHost: x\r\n\r\n"};
    char buf[16];
    size_t len = 0;
    int ret = run_head(pieces, 1, buf, sizeof(buf), &len);
    CHECK(ret == HEAD_TOO_LARGE, "버퍼가 가득 차도 \\r\\n\\r\\n이 없으면 HEAD_TOO_LARGE");
    CHECK(len == 15, "버퍼 크기 - 1(15바이트)까지만 받음");
    CHECK(buf[15] == '\0', "가득 찬 버퍼의 마지막 바이트는 '\\0'");

    /* 헤더가 버퍼에 꼭 맞게 들어가는 경계: 15바이트 요청과 16바이트 버퍼 */
    const char *const exact[] = {"GET / H\r\nA:\r\n\r\n"};
    ret = run_head(exact, 1, buf, sizeof(buf), &len);
    CHECK(ret == HEAD_OK && len == 15, "헤더가 버퍼 크기 - 1에 꼭 맞으면 HEAD_OK");
}

static void test_head_bad_args(void)
{
    char buf[8];
    size_t len = 0;
    CHECK(read_request_head(-1, NULL, 8, &len) == HEAD_ERROR, "buf가 NULL이면 HEAD_ERROR");
    CHECK(read_request_head(-1, buf, sizeof(buf), NULL) == HEAD_ERROR, "len이 NULL이면 HEAD_ERROR");
    CHECK(read_request_head(-1, buf, 1, &len) == HEAD_ERROR, "size가 1이면 HEAD_ERROR ('\\0' 말고는 받을 자리가 없음)");
}

/* build_file_path: 요청 대상을 파일 경로로 바꾼다 */
static void test_build_file_path(void)
{
    char path[64];

    CHECK(build_file_path("./www", "/img/red.png", path, sizeof(path)) == 0 && strcmp(path, "./www/img/red.png") == 0,
          "\"/img/red.png\"는 \"./www/img/red.png\"가 됨");
    CHECK(build_file_path("./www", "/", path, sizeof(path)) == 0 && strcmp(path, "./www/index.html") == 0,
          "\"/\"는 '/'로 끝나므로 \"./www/index.html\"이 됨");
    CHECK(build_file_path("./www", "/img/", path, sizeof(path)) == 0 && strcmp(path, "./www/img/index.html") == 0,
          "\"/img/\"도 '/'로 끝나므로 \"./www/img/index.html\"이 됨");
    CHECK(build_file_path("./www", "/index.html", path, sizeof(path)) == 0 && strcmp(path, "./www/index.html") == 0,
          "\"/index.html\"은 '/'로 끝나지 않으므로 아무것도 덧붙이지 않음");
    CHECK(build_file_path("./www", "", path, sizeof(path)) == 0 && strcmp(path, "./www") == 0,
          "요청 대상이 빈 문자열이면 root만 남음 (마지막 글자를 보지 않음)");

    /* "./www/abc"는 9글자다. '\0'까지 10바이트가 필요하다 */
    char small[10];
    CHECK(build_file_path("./www", "/abc", small, sizeof(small)) == 0 && strcmp(small, "./www/abc") == 0,
          "결과가 버퍼 크기 - 1글자에 꼭 맞으면 0을 반환");
    char smaller[9];
    CHECK(build_file_path("./www", "/abc", smaller, sizeof(smaller)) == -1, "결과가 버퍼보다 1글자 길면 -1을 반환");
    CHECK(build_file_path("./www", "/", smaller, sizeof(smaller)) == -1,
          "index.html을 덧붙인 결과가 버퍼보다 길어도 -1을 반환");
}

/* split_target: 요청 대상을 경로와 쿼리 문자열로 나눈다 */
static void test_split_target(void)
{
    char path[32];
    const char *query = NULL;

    CHECK(split_target("/search.html?q=abc", path, sizeof(path), &query) == 0 && strcmp(path, "/search.html") == 0 &&
              strcmp(query, "q=abc") == 0,
          "\"/search.html?q=abc\"는 경로 \"/search.html\", 쿼리 \"q=abc\"");
    CHECK(split_target("/index.html", path, sizeof(path), &query) == 0 && strcmp(path, "/index.html") == 0 &&
              strcmp(query, "") == 0,
          "'?'가 없으면 쿼리는 빈 문자열");
    CHECK(split_target("/a?", path, sizeof(path), &query) == 0 && strcmp(path, "/a") == 0 && strcmp(query, "") == 0,
          "'?'로 끝나면 쿼리는 빈 문자열");
    CHECK(split_target("/a?x=1?y=2", path, sizeof(path), &query) == 0 && strcmp(query, "x=1?y=2") == 0,
          "첫 번째 '?'에서만 나눔");
    char small[4];
    CHECK(split_target("/abc?x=1", small, sizeof(small), &query) == -1, "경로가 버퍼에 들어가지 않으면 -1");
    CHECK(split_target("/ab?x=1", small, sizeof(small), &query) == 0 && strcmp(small, "/ab") == 0,
          "경로가 버퍼에 꼭 맞으면 0");
}

/* path_has_dotdot: 경로에 ".." 칸이 있는지 본다 */
static void test_path_has_dotdot(void)
{
    CHECK(path_has_dotdot("/../Makefile") == 1, "\"/../Makefile\"에는 .. 칸이 있음");
    CHECK(path_has_dotdot("/a/../b") == 1, "경로 중간의 .. 칸");
    CHECK(path_has_dotdot("/a/..") == 1, "경로 끝의 .. 칸");
    CHECK(path_has_dotdot("..") == 1, "\"..\"만 있는 경우");
    CHECK(path_has_dotdot("/index.html") == 0, "평범한 경로에는 없음");
    CHECK(path_has_dotdot("/a..b/c") == 0, "칸의 일부가 ..인 것은 해당하지 않음");
    CHECK(path_has_dotdot("/.../x") == 0, "점 세 개는 해당하지 않음");
    CHECK(path_has_dotdot("/./x") == 0, "점 한 개는 해당하지 않음");
    CHECK(path_has_dotdot("/") == 0 && path_has_dotdot("") == 0, "\"/\"와 빈 문자열에는 없음");
}

int main(void)
{
    test_curl_request();
    test_request_line_only();
    test_result_is_a_copy();
    test_bad_request_lines();
    test_long_target();
    test_free_request();
    test_head_one_piece();
    test_head_split();
    test_head_closed();
    test_head_too_large();
    test_head_bad_args();
    test_build_file_path();
    test_split_target();
    test_path_has_dotdot();

    if (failures > 0) {
        printf("단위 테스트 실패: %d개\n", failures);
        return 1;
    }
    printf("단위 테스트 모두 통과\n");
    return 0;
}
