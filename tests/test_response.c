/*
 * response.c 단위 테스트. 외부 테스트 라이브러리 없이 표준 C로만 작성한다.
 * 각 테스트는 실패하면 FAIL 줄을 출력하고, 실패가 하나라도 있으면 종료 코드 1.
 */
#include "response.h"

#include <stdio.h>
#include <string.h>     /* strcmp, strstr */
#include <sys/socket.h> /* socketpair, AF_UNIX, SOCK_STREAM */
#include <sys/types.h>  /* ssize_t */
#include <unistd.h>     /* read, close */

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

/* mime_type: 확장자로 Content-Type 값을 정한다 */
static void test_mime_type(void)
{
    CHECK(strcmp(mime_type("./www/index.html"), "text/html; charset=utf-8") == 0, ".html은 text/html");
    CHECK(strcmp(mime_type("./www/style.css"), "text/css; charset=utf-8") == 0, ".css는 text/css");
    CHECK(strcmp(mime_type("./www/app.js"), "text/javascript; charset=utf-8") == 0, ".js는 text/javascript");
    CHECK(strcmp(mime_type("./www/img/red.png"), "image/png") == 0, ".png는 image/png");
    CHECK(strcmp(mime_type("./www/img/blue.jpg"), "image/jpeg") == 0, ".jpg는 image/jpeg");
    CHECK(strcmp(mime_type("a.jpeg"), "image/jpeg") == 0, ".jpeg도 image/jpeg");
    CHECK(strcmp(mime_type("A.PNG"), "image/png") == 0, "확장자의 대소문자를 구분하지 않음");
    CHECK(strcmp(mime_type("a.tar.png"), "image/png") == 0, "점이 여러 개면 마지막 점부터가 확장자");
    CHECK(strcmp(mime_type("./www/readme"), "application/octet-stream") == 0, "확장자가 없으면 application/octet-stream");
    CHECK(strcmp(mime_type("./www/a.xyz"), "application/octet-stream") == 0, "모르는 확장자도 application/octet-stream");
    CHECK(strcmp(mime_type("./v1.2/readme"), "application/octet-stream") == 0, "폴더 이름의 점은 확장자가 아님");
}

/* status_text: 상태 코드의 설명 문구 */
static void test_status_text(void)
{
    CHECK(strcmp(status_text(200), "OK") == 0, "200은 OK");
    CHECK(strcmp(status_text(400), "Bad Request") == 0, "400은 Bad Request");
    CHECK(strcmp(status_text(403), "Forbidden") == 0, "403은 Forbidden");
    CHECK(strcmp(status_text(404), "Not Found") == 0, "404는 Not Found");
    CHECK(strcmp(status_text(405), "Method Not Allowed") == 0, "405는 Method Not Allowed");
    CHECK(strcmp(status_text(500), "Internal Server Error") == 0, "500은 Internal Server Error");
    CHECK(strcmp(status_text(501), "Not Implemented") == 0, "501은 Not Implemented");
    CHECK(strcmp(status_text(505), "HTTP Version Not Supported") == 0, "505는 HTTP Version Not Supported");
    CHECK(strcmp(status_text(999), "Unknown") == 0, "표에 없는 코드는 Unknown");
}

/*
 * send_error가 보내는 바이트를 확인한다.
 * socketpair는 서로 연결된 소켓 두 개를 만든다. sv[0]에 쓴 바이트를 sv[1]에서 읽을 수 있다.
 */
static void test_send_error(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
        perror("socketpair");
        failures++;
        return;
    }
    char buf[1024];

    CHECK(send_error(sv[0], 404) == 404, "send_error는 보낸 상태 코드를 돌려줌");
    ssize_t n = read(sv[1], buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    const char *expected = "HTTP/1.1 404 Not Found\r\n"
                           "Content-Type: text/html; charset=utf-8\r\n"
                           "Content-Length: 23\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "<h1>404 Not Found</h1>\n";
    CHECK(strcmp(buf, expected) == 0, "404 응답의 바이트가 기대와 같음 (본문 23바이트)");

    send_error(sv[0], 405);
    n = read(sv[1], buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    CHECK(strstr(buf, "\r\nAllow: GET\r\n") != NULL, "405 응답에는 Allow: GET 헤더가 있음");

    send_error(sv[0], 500);
    n = read(sv[1], buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    CHECK(strstr(buf, "Allow:") == NULL, "405가 아닌 응답에는 Allow 헤더가 없음");

    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    test_mime_type();
    test_status_text();
    test_send_error();

    if (failures > 0) {
        printf("단위 테스트 실패: %d개\n", failures);
        return 1;
    }
    printf("단위 테스트 모두 통과\n");
    return 0;
}
