#include "handler.h"

#include "cgi.h"
#include "http.h"
#include "response.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQ_BUF_SIZE 8192 /* 요청 줄과 헤더를 받는 버퍼 크기 */
#define PATH_BUF_SIZE 4096 /* 파일 경로 버퍼 크기 */
#define HEADER_VALUE_SIZE 256 /* 헤더 값 하나를 담는 버퍼 크기 */
#define MAX_BODY_SIZE (1024 * 1024) /* 요청 본문 최대 크기(1MiB). 넘으면 413 */

#define CGI_PREFIX "/cgi-bin/" /* 경로가 이 글자들로 시작하면 CGI 프로그램을 실행한다 */

#define NO_RESPONSE 0 /* respond의 반환값: 응답을 보내지 않았다 */

/*
 * POST 본문을 받아 malloc으로 할당한 메모리에 담는다. 성공하면 200을 돌려주고, 호출한 쪽이 *body를 free해야 한다.
 * 실패하면 에러 응답(400, 411, 413, 500)을 보내고 그 코드를 돌려준다. 연결이 끊기면 NO_RESPONSE.
 */
static int receive_body(int conn_fd, const char *head, size_t head_len, char **body, size_t *body_len)
{
    *body = NULL;
    *body_len = 0;

    char value[HEADER_VALUE_SIZE];
    /* 본문에는 끝 표시가 없어서 Content-Length가 없으면 길이를 알 수 없다 */
    int found = get_header(head, "Content-Length", value, sizeof(value));
    if (found == 1) {
        return send_error(conn_fd, 411);
    }
    size_t content_length;
    if (found == -1 || parse_content_length(value, &content_length) == -1) {
        return send_error(conn_fd, 400);
    }

    if (content_length > MAX_BODY_SIZE) {
        return send_error(conn_fd, 413);
    }

    char *buf = malloc(content_length + 1);
    if (buf == NULL) {
        perror("malloc");
        return send_error(conn_fd, 500);
    }

    /* 헤더를 읽을 때 본문 앞부분이 같은 버퍼에 들어왔을 수 있다. 빈 줄 다음부터가 그 부분이다 */
    const char *body_start = strstr(head, "\r\n\r\n") + 4;
    size_t already_len = head_len - (size_t)(body_start - head);

    int result = read_body(conn_fd, body_start, already_len, buf, content_length);
    if (result != BODY_OK) {
        free(buf);
        printf("본문을 다 받기 전에 연결이 끊겼습니다 (Content-Length %zu)\n", content_length);
        return NO_RESPONSE;
    }
    buf[content_length] = '\0';
    *body = buf;
    *body_len = content_length;
    return 200;
}

/* 요청을 검사하고 응답을 보낸다. 보낸 상태 코드를 돌려준다 */
static int respond(int conn_fd, const char *root, const struct http_request *req, const char *head, size_t head_len)
{
    int is_http11 = strcmp(req->version, "HTTP/1.1") == 0;
    if (!is_http11 && strcmp(req->version, "HTTP/1.0") != 0) {
        return send_error(conn_fd, 505);
    }

    int is_post = strcmp(req->method, "POST") == 0;
    if (!is_post && strcmp(req->method, "GET") != 0) {
        return send_error(conn_fd, 501);
    }

    char value[HEADER_VALUE_SIZE];
    /* HTTP/1.1 요청에는 Host 헤더가 반드시 있어야 한다 */
    int host_found = get_header(head, "Host", value, sizeof(value));
    if (host_found == 0) {
        printf("Host 헤더: %s\n", value);
    } else if (is_http11) {
        return send_error(conn_fd, 400);
    }
    if (get_header(head, "Connection", value, sizeof(value)) == 0) {
        printf("Connection 헤더: %s\n", value);
    }

    if (req->target[0] != '/') {
        return send_error(conn_fd, 400);
    }

    char url_path[PATH_BUF_SIZE];
    const char *query;
    if (split_target(req->target, url_path, sizeof(url_path), &query) == -1) {
        return send_error(conn_fd, 414);
    }

    /* ".."으로 정적 파일 폴더 밖을 가리키지 못하게 한다 */
    if (path_has_dotdot(url_path)) {
        return send_error(conn_fd, 403);
    }

    /* 본문을 읽지 않고 닫으면 커널이 연결을 RST로 끊을 수 있으므로, 응답하기 전에 본문을 다 받는다 */
    char *body = NULL;
    size_t body_len = 0;
    if (is_post) {
        int status = receive_body(conn_fd, head, head_len, &body, &body_len);
        if (status != 200) {
            return status;
        }

        printf("본문 %zu바이트: %.*s\n", body_len, body_len > 200 ? 200 : (int)body_len, body);
    }

    char file_path[PATH_BUF_SIZE];
    if (build_file_path(root, url_path, file_path, sizeof(file_path)) == -1) {
        free(body);
        return send_error(conn_fd, 414);
    }
    printf("파일 경로: %s\n", file_path);

    /* 동적 요청: 파일을 보내는 대신 프로그램으로 실행한다 */
    if (strncmp(url_path, CGI_PREFIX, sizeof(CGI_PREFIX) - 1) == 0) {
        int status = run_cgi(conn_fd, file_path, req->method, query, body, body_len);
        free(body);
        return status;
    }

    /* 정적 파일은 GET만 받는다 */
    if (is_post) {
        free(body);
        return send_error(conn_fd, 405);
    }

    return send_file(conn_fd, file_path);
}

void handle_connection(int conn_fd, const char *root)
{
    char buf[REQ_BUF_SIZE];
    size_t len = 0;
    int head = read_request_head(conn_fd, buf, sizeof(buf), &len);
    printf("받은 바이트 수: %zu\n", len);

    if (head == HEAD_ERROR) {
        return;
    }
    if (head == HEAD_CLOSED) {
        printf("헤더가 끝나기 전에 연결이 닫혔습니다\n");
        return;
    }
    if (head == HEAD_TOO_LARGE) {
        send_error(conn_fd, 431);
        printf("응답: 431 (헤더가 버퍼 %d바이트보다 김)\n", REQ_BUF_SIZE);
        return;
    }

    struct http_request req;
    if (parse_request_line(buf, &req) == -1) {
        send_error(conn_fd, 400);
        printf("응답: 400 (요청 줄 형식이 맞지 않음)\n");
        return;
    }
    printf("요청 줄: %s %s %s\n", req.method, req.target, req.version);

    int status = respond(conn_fd, root, &req, buf, len);
    if (status == NO_RESPONSE) {
        printf("응답: 보내지 않음\n");
    } else {
        printf("응답: %d %s\n", status, status_text(status));
    }

    free_request(&req);
}
