#include "handler.h"

#include "cgi.h"
#include "http.h"
#include "response.h"

#include <stddef.h> /* size_t */
#include <stdio.h>  /* printf, perror */
#include <stdlib.h> /* malloc, free */
#include <string.h> /* strcmp, strncmp, strstr */

#define REQ_BUF_SIZE 8192  /* 요청 줄과 헤더를 받는 버퍼 크기(바이트). 마지막 한 바이트는 '\0' 자리로 남긴다 */
#define PATH_BUF_SIZE 4096 /* 파일 경로를 담는 버퍼 크기(바이트). 리눅스가 받아 주는 경로 최대 길이(PATH_MAX)와 같다 */
#define HEADER_VALUE_SIZE 256        /* 헤더 값 하나를 담는 버퍼 크기(바이트) */
#define MAX_BODY_SIZE (1024 * 1024)  /* 받아 주는 요청 본문의 최대 크기: 1MiB. 넘으면 413 */

#define CGI_PREFIX "/cgi-bin/" /* 경로가 이 글자들로 시작하면 정적 파일이 아니라 CGI 프로그램을 실행한다 */

#define NO_RESPONSE 0 /* respond의 반환값: 응답을 보내지 않았다 (클라이언트가 먼저 연결을 닫음) */

/*
 * POST 요청의 본문을 받는다.
 * head: 요청 줄과 헤더 전체가 담긴 버퍼, head_len: 그 버퍼에 받은 바이트 수 (본문 앞부분이 섞여 있을 수 있다).
 * 성공하면 malloc으로 할당한 본문의 주소를 *body에, 길이를 *body_len에 넣고 200을 돌려준다.
 *   본문은 호출한 쪽이 free로 해제해야 한다. 출력하기 쉽게 끝에 '\0'을 하나 붙여 둔다 (길이에는 세지 않는다).
 * 실패하면 에러 응답을 보내고 그 상태 코드(400, 411, 413, 500)를 돌려준다. 이때 *body는 NULL이다.
 * 본문을 다 받기 전에 연결이 닫히면 응답을 보내지 않고 NO_RESPONSE를 돌려준다.
 */
static int receive_body(int conn_fd, const char *head, size_t head_len, char **body, size_t *body_len)
{
    *body = NULL;
    *body_len = 0;

    /* 1) Content-Length 헤더를 찾는다. 본문에는 끝 표시가 없어서, 이 헤더가 없으면 본문이 어디까지인지 알 수 없다 */
    char value[HEADER_VALUE_SIZE];
    int found = get_header(head, "Content-Length", value, sizeof(value));
    if (found == 1) {
        return send_error(conn_fd, 411); /* 411 Length Required: 길이를 알려 달라 */
    }
    size_t content_length;
    if (found == -1 || parse_content_length(value, &content_length) == -1) {
        return send_error(conn_fd, 400); /* 값이 숫자가 아니다 */
    }

    /* 2) 너무 큰 본문은 받지 않는다. 받기 전에 헤더의 숫자만 보고 거절하므로 메모리를 쓰지 않는다 */
    if (content_length > MAX_BODY_SIZE) {
        return send_error(conn_fd, 413);
    }

    /* 3) 본문을 담을 메모리를 할당한다. 크기가 요청마다 다르므로 malloc을 쓴다 */
    char *buf = malloc(content_length + 1);
    if (buf == NULL) {
        perror("malloc");
        return send_error(conn_fd, 500);
    }

    /*
     * 4) 본문은 빈 줄("\r\n\r\n") 바로 다음 바이트부터다.
     * 헤더를 읽을 때 본문 앞부분이 같은 버퍼에 함께 들어왔을 수 있다. 그 길이(already_len)는
     * 받은 전체 바이트 수에서 "본문 시작 위치까지의 바이트 수"를 뺀 값이다.
     */
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

/*
 * 나눠 놓은 요청 줄(req)과 헤더(head)를 검사하고 응답을 보낸다. 보낸 상태 코드를 돌려준다.
 * 검사에서 걸리면 그 자리에서 에러 응답을 보내고 돌아간다 (send_error는 보낸 코드를 그대로 돌려준다).
 */
static int respond(int conn_fd, const char *root, const struct http_request *req, const char *head, size_t head_len)
{
    /* 1) HTTP 버전. 이 서버가 아는 버전은 HTTP/1.0과 HTTP/1.1뿐이다. strcmp는 두 문자열이 같으면 0을 돌려준다 */
    int is_http11 = strcmp(req->version, "HTTP/1.1") == 0;
    if (!is_http11 && strcmp(req->version, "HTTP/1.0") != 0) {
        return send_error(conn_fd, 505);
    }

    /* 2) 메서드. 이 서버가 구현한 메서드는 GET과 POST뿐이다. 그 밖은 501 Not Implemented */
    int is_post = strcmp(req->method, "POST") == 0;
    if (!is_post && strcmp(req->method, "GET") != 0) {
        return send_error(conn_fd, 501);
    }

    /*
     * 3) 헤더. Host, Connection 값을 찾아 출력한다.
     * HTTP/1.1 요청에는 Host 헤더가 반드시 있어야 한다 (RFC 9112 3.2절). 없으면 400.
     * Connection 값(keep-alive, close)은 읽기만 한다. 이 서버는 값과 상관없이 응답 뒤에 연결을 닫는다.
     */
    char value[HEADER_VALUE_SIZE];
    int host_found = get_header(head, "Host", value, sizeof(value));
    if (host_found == 0) {
        printf("Host 헤더: %s\n", value);
    } else if (is_http11) {
        return send_error(conn_fd, 400);
    }
    if (get_header(head, "Connection", value, sizeof(value)) == 0) {
        printf("Connection 헤더: %s\n", value);
    }

    /* 4) 요청 대상은 '/'로 시작해야 한다. 그렇지 않으면 파일 경로로 바꿀 수 없다 (예: "*", "http://...") */
    if (req->target[0] != '/') {
        return send_error(conn_fd, 400);
    }

    /* 5) '?' 뒤의 쿼리 문자열을 떼고 경로만 남긴다 */
    char url_path[PATH_BUF_SIZE];
    const char *query;
    if (split_target(req->target, url_path, sizeof(url_path), &query) == -1) {
        return send_error(conn_fd, 414);
    }

    /* 6) ".." 칸이 있으면 정적 파일 폴더 밖을 가리킬 수 있으므로 거절한다 */
    if (path_has_dotdot(url_path)) {
        return send_error(conn_fd, 403);
    }

    /*
     * 7) POST면 본문을 받는다. 실패하면 receive_body가 에러 응답을 이미 보냈으므로 그 코드를 그대로 돌려준다.
     * 본문을 읽지 않은 채 응답하고 닫으면, 커널이 "읽지 않은 데이터가 있는 연결"을 강제로 끊어(RST)
     * 클라이언트 프로세스가 응답을 못 받을 수 있다. 그래서 어떤 응답을 보내든 먼저 본문을 다 읽는다.
     */
    char *body = NULL;
    size_t body_len = 0;
    if (is_post) {
        int status = receive_body(conn_fd, head, head_len, &body, &body_len);
        if (status != 200) {
            return status;
        }
        /* %.*s: 문자열에서 앞의 정해진 글자 수까지만 출력한다. 긴 본문은 앞 200바이트만 보여 준다 */
        printf("본문 %zu바이트: %.*s\n", body_len, body_len > 200 ? 200 : (int)body_len, body);
    }

    /* 8) 정적 파일 폴더를 앞에 붙여 파일 경로를 만든다. '/'로 끝나면 index.html이 붙는다 */
    char file_path[PATH_BUF_SIZE];
    if (build_file_path(root, url_path, file_path, sizeof(file_path)) == -1) {
        free(body); /* free(NULL)은 아무 일도 하지 않으므로 GET이어도 그대로 불러도 된다 */
        return send_error(conn_fd, 414);
    }
    printf("파일 경로: %s\n", file_path);

    /*
     * 9) 경로가 "/cgi-bin/"으로 시작하면 그 파일을 보내는 대신 프로그램으로 실행한다 (동적 요청).
     * strncmp(a, b, n): 앞의 n글자만 비교한다 (같으면 0). sizeof(CGI_PREFIX) - 1은 끝의 '\0'을 뺀 글자 수 9다.
     * 메서드, 쿼리 문자열, 본문을 넘기고, 프로그램의 출력을 응답으로 보낸다. GET과 POST 모두 받는다.
     */
    if (strncmp(url_path, CGI_PREFIX, sizeof(CGI_PREFIX) - 1) == 0) {
        int status = run_cgi(conn_fd, file_path, req->method, query, body, body_len);
        free(body);
        return status;
    }

    /* 10) 정적 파일은 GET으로만 받는다. POST면 405 */
    if (is_post) {
        free(body);
        return send_error(conn_fd, 405);
    }

    /* 11) 파일을 열어 보낸다. 파일이 없거나 열 수 없으면 send_file이 404, 403, 500 중 하나를 보낸다 */
    return send_file(conn_fd, file_path);
}

void handle_connection(int conn_fd, const char *root)
{
    /*
     * 헤더의 끝(빈 줄, "\r\n\r\n")까지 요청을 읽는다.
     * read_request_head는 "\r\n\r\n"이 나올 때까지 read를 반복해 buf에 이어 붙이고, 끝에 '\0'을 넣어 준다.
     * POST 요청이면 빈 줄 뒤의 본문 앞부분이 buf에 함께 들어와 있을 수 있다.
     */
    char buf[REQ_BUF_SIZE];
    size_t len = 0;
    int head = read_request_head(conn_fd, buf, sizeof(buf), &len);
    printf("받은 바이트 수: %zu\n", len);

    if (head == HEAD_ERROR) {
        return; /* read가 실패했다. 응답을 보내지 않고 돌아간다 */
    }
    if (head == HEAD_CLOSED) {
        /* 헤더가 끝나기 전에 클라이언트가 연결을 닫았다. 응답을 받을 상대가 없다 */
        printf("헤더가 끝나기 전에 연결이 닫혔습니다\n");
        return;
    }
    if (head == HEAD_TOO_LARGE) {
        /* 요청 줄과 헤더가 버퍼보다 길다. 요청을 해석하지 않고 431을 보낸다 */
        send_error(conn_fd, 431);
        printf("응답: 431 (헤더가 버퍼 %d바이트보다 김)\n", REQ_BUF_SIZE);
        return;
    }

    /*
     * 요청 줄을 메서드, 요청 대상, 버전 세 칸으로 나눈다 (단계 2의 파서).
     * 실패하면 요청 줄의 형식이 틀린 것이므로 400을 보낸다.
     * 성공하면 세 칸은 malloc으로 할당된 메모리에 있으므로, 다 쓴 뒤 free_request로 해제한다.
     */
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
