#include "handler.h"

#include "http.h"
#include "response.h"

#include <stddef.h> /* size_t */
#include <stdio.h>  /* printf */
#include <string.h> /* strcmp */

#define REQ_BUF_SIZE 8192  /* 요청 줄과 헤더를 받는 버퍼 크기(바이트). 마지막 한 바이트는 '\0' 자리로 남긴다 */
#define PATH_BUF_SIZE 4096 /* 파일 경로를 담는 버퍼 크기(바이트). 리눅스가 받아 주는 경로 최대 길이(PATH_MAX)와 같다 */

/*
 * 나눠 놓은 요청 줄(req)을 검사하고 응답을 보낸다. 보낸 상태 코드를 돌려준다.
 * 검사에서 걸리면 그 자리에서 에러 응답을 보내고 돌아간다 (send_error는 보낸 코드를 그대로 돌려준다).
 */
static int respond(int conn_fd, const char *root, const struct http_request *req)
{
    /* 1) HTTP 버전. 이 서버가 아는 버전은 HTTP/1.0과 HTTP/1.1뿐이다. strcmp는 두 문자열이 같으면 0을 돌려준다 */
    if (strcmp(req->version, "HTTP/1.1") != 0 && strcmp(req->version, "HTTP/1.0") != 0) {
        return send_error(conn_fd, 505);
    }

    /*
     * 2) 메서드. 정적 파일은 GET으로만 받는다.
     *   POST : 서버가 아는 메서드지만 정적 파일에는 허용하지 않는다 → 405 Method Not Allowed
     *   그 밖: 서버가 구현하지 않은 메서드다 (예: PUT, DELETE, 없는 이름) → 501 Not Implemented
     */
    if (strcmp(req->method, "GET") != 0) {
        if (strcmp(req->method, "POST") == 0) {
            return send_error(conn_fd, 405);
        }
        return send_error(conn_fd, 501);
    }

    /* 3) 요청 대상은 '/'로 시작해야 한다. 그렇지 않으면 파일 경로로 바꿀 수 없다 (예: "*", "http://...") */
    if (req->target[0] != '/') {
        return send_error(conn_fd, 400);
    }

    /* 4) '?' 뒤의 쿼리 문자열을 떼고 경로만 남긴다. 정적 파일을 찾을 때 쿼리 문자열은 쓰지 않는다 */
    char url_path[PATH_BUF_SIZE];
    const char *query;
    if (split_target(req->target, url_path, sizeof(url_path), &query) == -1) {
        return send_error(conn_fd, 414);
    }

    /* 5) ".." 칸이 있으면 정적 파일 폴더 밖을 가리킬 수 있으므로 거절한다 */
    if (path_has_dotdot(url_path)) {
        return send_error(conn_fd, 403);
    }

    /* 6) 정적 파일 폴더를 앞에 붙여 파일 경로를 만든다. '/'로 끝나면 index.html이 붙는다 */
    char file_path[PATH_BUF_SIZE];
    if (build_file_path(root, url_path, file_path, sizeof(file_path)) == -1) {
        return send_error(conn_fd, 414);
    }
    printf("파일 경로: %s\n", file_path);

    /* 7) 파일을 열어 보낸다. 파일이 없거나 열 수 없으면 send_file이 404, 403, 500 중 하나를 보낸다 */
    return send_file(conn_fd, file_path);
}

void handle_connection(int conn_fd, const char *root)
{
    /*
     * 헤더의 끝(빈 줄, "\r\n\r\n")까지 요청을 읽는다.
     * read_request_head는 "\r\n\r\n"이 나올 때까지 read를 반복해 buf에 이어 붙이고, 끝에 '\0'을 넣어 준다.
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

    int status = respond(conn_fd, root, &req);
    printf("응답: %d %s\n", status, status_text(status));

    free_request(&req);
}
