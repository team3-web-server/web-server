#include "handler.h"

#include "cgi.h"
#include "http.h"
#include "response.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>

#define REQ_BUF_SIZE 8192 /* 요청 줄과 헤더를 받는 버퍼 크기 */
#define PATH_BUF_SIZE 4096 /* 파일 경로 버퍼 크기 */
#define HEADER_VALUE_SIZE 256 /* 헤더 값 하나를 담는 버퍼 크기 */
#define MAX_BODY_SIZE (1024 * 1024) /* 요청 본문 최대 크기(1MiB). 넘으면 413 */

#define CGI_PREFIX "/cgi-bin/" /* 경로가 이 글자들로 시작하면 CGI 프로그램을 실행한다 */

#define NO_RESPONSE 0 /* respond의 반환값: 응답을 보내지 않았다 */
#define IDLE_TIMEOUT_SEC 5 /* 이 시간 동안 다음 요청이 오지 않으면 연결을 닫는다 */

/*
 * POST 본문을 받아 malloc으로 할당한 메모리에 담는다. 성공하면 200을 돌려주고, 호출한 쪽이 *body를 free해야 한다.
 * 실패하면 에러 응답(400, 411, 413, 500)을 보내고 그 코드를 돌려준다. 연결이 끊기면 NO_RESPONSE.
 * 버퍼(head)에 본문보다 더 많은 바이트가 들어와 있으면, *used를 이 요청이 쓴 바이트 수(요청 줄 + 헤더 + 본문)로 고친다.
 */
static int receive_body(int conn_fd, const char *head, size_t head_len, char **body, size_t *body_len, size_t *used)
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
    /* 버퍼에 본문보다 더 들어와 있으면 그 뒤는 다음 요청의 앞부분이다 */
    if (already_len > content_length) {
        *used = (size_t)(body_start - head) + content_length;
    }
    *body = buf;
    *body_len = content_length;
    return 200;
}

/*
 * 이 요청의 응답 뒤에도 연결을 유지할지 정한다 (RFC 9112 9.3절).
 * HTTP/1.1은 유지가 기본이고 "Connection: close"가 있으면 닫는다.
 * HTTP/1.0은 닫기가 기본이고 "Connection: keep-alive"가 있으면 유지한다.
 */
static int wants_keep_alive(const char *head, int is_http11)
{
    char value[HEADER_VALUE_SIZE];
    if (get_header(head, "Connection", value, sizeof(value)) != 0) {
        return is_http11;
    }
    printf("Connection 헤더: %s\n", value);
    if (strcasecmp(value, "close") == 0) {
        return 0;
    }
    if (strcasecmp(value, "keep-alive") == 0) {
        return 1;
    }
    return is_http11;
}

/*
 * 요청을 검사하고 응답을 보낸다. 보낸 상태 코드를 돌려준다.
 * keep_alive: 200 응답에 넣을 Connection 헤더를 정한다 (1이면 keep-alive, 0이면 close).
 * *used: head 버퍼에서 이 요청이 쓴 바이트 수. 그 뒤에 남은 바이트는 다음 요청의 앞부분이다.
 */
static int respond(int conn_fd, const char *root, const struct http_request *req, const char *head, size_t head_len,
                   int keep_alive, size_t *used)
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
        int status = receive_body(conn_fd, head, head_len, &body, &body_len, used);
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
        int status = run_cgi(conn_fd, file_path, req->method, query, body, body_len, keep_alive);
        free(body);
        return status;
    }

    /* 정적 파일은 GET만 받는다 */
    if (is_post) {
        free(body);
        return send_error(conn_fd, 405);
    }

    return send_file(conn_fd, file_path, keep_alive);
}

void handle_connection(int conn_fd, const char *root)
{
    /* 다음 요청을 기다리는 read가 IDLE_TIMEOUT_SEC초 넘게 멈춰 있으면 -1(EAGAIN)로 돌아오게 한다 */
    struct timeval timeout = {IDLE_TIMEOUT_SEC, 0};
    if (setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt");
    }

    /*
     * 응답은 헤더와 본문을 write 두 번으로 보낸다. 커널은 작은 데이터를 모아 보내려고(Nagle 알고리즘) 앞 write의 ACK가
     * 올 때까지 둘째 write를 보내지 않고, 클라이언트 쪽 커널은 ACK를 약 40ms 늦춰 보낸다. 유지된 연결의 두 번째 요청부터
     * 응답이 40ms씩 늦어지므로, TCP_NODELAY로 write한 것을 바로 보내게 한다.
     */
    int on = 1;
    if (setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == -1) {
        perror("setsockopt");
    }

    /* buf와 len은 반복문 밖에 둔다. 앞 요청과 함께 들어온 다음 요청의 앞부분을 다음 바퀴에서 이어 쓴다 */
    char buf[REQ_BUF_SIZE];
    size_t len = 0;

    /* keep-alive: 같은 연결(conn_fd)에서 [요청 읽기 → 응답 쓰기]를 반복한다 */
    for (int count = 1;; count++) {
        int head = read_request_head(conn_fd, buf, sizeof(buf), &len);
        if (head == HEAD_TIMEOUT) {
            printf("%d초 동안 요청이 없어 연결을 닫습니다\n", IDLE_TIMEOUT_SEC);
            return;
        }
        if (head == HEAD_CLOSED && len == 0 && count > 1) {
            printf("클라이언트가 연결을 닫았습니다\n");
            return;
        }
        printf("이 연결의 %d번째 요청, 받은 바이트 수: %zu\n", count, len);

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

        int keep_alive = wants_keep_alive(buf, strcmp(req.version, "HTTP/1.1") == 0);
        /* 본문이 없는 요청(GET)은 빈 줄까지가 이 요청이다. POST는 버퍼 전체로 두고, 더 들어와 있으면 receive_body가 고친다 */
        size_t used = (size_t)(strstr(buf, "\r\n\r\n") + 4 - buf);
        if (strcmp(req.method, "POST") == 0) {
            used = len;
        }

        int status = respond(conn_fd, root, &req, buf, len, keep_alive, &used);
        free_request(&req);
        if (status == NO_RESPONSE) {
            printf("응답: 보내지 않음\n");
            return;
        }
        printf("응답: %d %s\n", status, status_text(status));

        /* 에러 응답은 항상 "Connection: close"로 보냈으므로 연결을 닫는다 */
        if (status != 200 || !keep_alive) {
            return;
        }
        printf("연결을 유지하고 다음 요청을 기다립니다\n");

        /* 이 요청이 쓴 만큼을 버리고, 남은 바이트(다음 요청의 앞부분)를 buf의 맨 앞으로 옮긴다 */
        len -= used;
        memmove(buf, buf + used, len);
    }
}
