#ifndef HTTP_H
#define HTTP_H

/* HTTP 요청을 읽고 해석하는 함수 모음 */

#include <stddef.h>

/* 요청 줄을 나눈 결과. 세 칸은 malloc으로 할당되므로 다 쓰면 free_request로 해제한다 */
struct http_request {
    char *method; /* 예: "GET" */
    char *target; /* 예: "/index.html" */
    char *version; /* 예: "HTTP/1.1" */
};

/*
 * buf의 첫 줄을 "메서드 SP 요청 대상 SP 버전 CRLF" 형식으로 보고 세 칸으로 나눠 req에 넣는다.
 * 성공하면 0. 형식이 틀리거나 malloc이 실패하면 -1이고 req의 세 포인터는 NULL이다.
 */
int parse_request_line(const char *buf, struct http_request *req);

/* req의 세 칸을 해제하고 NULL로 바꾼다. 여러 번 불러도 안전하다 */
void free_request(struct http_request *req);

/* read_request_head의 반환값 */
#define HEAD_OK 0 /* 빈 줄("\r\n\r\n")까지 받았다 */
#define HEAD_CLOSED 1 /* 그 전에 클라이언트가 연결을 닫았다 */
#define HEAD_TOO_LARGE 2 /* 버퍼가 가득 찼는데 빈 줄이 없다 */
#define HEAD_ERROR (-1) /* read 실패 */

/*
 * fd에서 빈 줄("\r\n\r\n")이 나올 때까지 read를 반복해 buf에 이어 붙인다. buf는 항상 '\0'으로 끝난다.
 * 요청 하나가 read 여러 번에 나뉘어 올 수 있어서, 받을 때마다 지금까지 받은 전체에서 빈 줄을 찾는다.
 * size는 buf의 크기이고, *len에는 받은 바이트 수가 들어간다.
 */
int read_request_head(int fd, char *buf, size_t size, size_t *len);

/*
 * root와 target을 이어 파일 경로를 path에 만든다. target이 '/'로 끝나면 "index.html"을 붙인다.
 * 예) "./www" + "/" → "./www/index.html".  path(크기 size)에 다 들어가지 않으면 -1.
 */
int build_file_path(const char *root, const char *target, char *path, size_t size);

/*
 * target을 '?' 기준으로 나눈다. 경로는 path에 복사하고, *query는 target 안의 '?' 다음 글자를 가리킨다.
 * 예) "/a.html?q=1" → 경로 "/a.html", 쿼리 문자열 "q=1".  경로가 path에 다 들어가지 않으면 -1.
 */
int split_target(const char *target, char *path, size_t size, const char **query);

/* '/'로 나눈 칸 중에 ".."이 있으면 1, 없으면 0. 예) "/../Makefile" → 1, "/a..b" → 0 */
int path_has_dotdot(const char *path);

/*
 * head(요청 줄 + 헤더)에서 이름이 name인 헤더의 값을 value에 복사한다. 이름은 대소문자를 구분하지 않는다.
 * 찾았으면 0, 없으면 1, 값이 value(크기 size)에 다 들어가지 않으면 -1.
 */
int get_header(const char *head, const char *name, char *value, size_t size);

/* get_header와 같지만 첫 줄을 건너뛰지 않는다. CGI 출력처럼 첫 줄부터 헤더인 글에 쓴다 */
int find_header(const char *lines, const char *name, char *value, size_t size);

/* Content-Length 값(text)을 숫자로 바꿔 *out에 넣는다. 숫자가 아닌 글자가 있거나 너무 크면 -1 */
int parse_content_length(const char *text, size_t *out);

/* read_body의 반환값 */
#define BODY_OK 0 /* 모두 받았다 */
#define BODY_CLOSED 1 /* 다 받기 전에 클라이언트가 연결을 닫았다 */
#define BODY_ERROR (-1) /* read 실패 */

/*
 * 본문을 content_length바이트만큼 body에 채운다.
 * 헤더와 함께 이미 들어온 부분(already, already_len)을 먼저 복사하고 모자란 만큼만 read한다.
 */
int read_body(int fd, const char *already, size_t already_len, char *body, size_t content_length);

#endif /* HTTP_H */
