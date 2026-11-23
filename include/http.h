#ifndef HTTP_H
#define HTTP_H

/*
 * HTTP 요청을 읽고 해석하는 함수 모음.
 * 단계 2: 요청의 첫 줄(요청 줄)을 세 칸으로 나누는 함수.
 * 단계 3: 연결 소켓에서 헤더의 끝(빈 줄, "\r\n\r\n")까지 읽는 함수.
 * 단계 4: 요청 대상을 파일 경로로 바꾸는 함수.
 * 단계 6: 헤더 값을 찾는 함수, Content-Length 값을 숫자로 바꾸는 함수.
 * 단계 7: 요청 본문을 Content-Length만큼 읽는 함수.
 * 단계 5: 요청 대상에서 쿼리 문자열을 떼는 함수, ".." 칸이 있는지 보는 함수.
 */

#include <stddef.h> /* size_t */

/*
 * 요청 줄을 나눈 결과. 세 칸 모두 '\0'으로 끝나는 C 문자열이고,
 * parse_request_line이 malloc으로 할당한 메모리를 가리킨다.
 * 다 쓰면 free_request로 해제해야 한다.
 */
struct http_request {
    char *method;  /* 메서드 (예: "GET") */
    char *target;  /* 요청 대상 (예: "/index.html") */
    char *version; /* HTTP 버전 (예: "HTTP/1.1") */
};

/*
 * buf의 첫 줄을 요청 줄로 보고 메서드, 요청 대상, 버전 세 칸으로 나눈다.
 * 각 칸의 길이 + 1바이트를 malloc으로 할당해 복사하고, 그 주소를 req에 넣는다.
 * 요청 줄 형식(RFC 9112 3절): method SP request-target SP HTTP-version CRLF
 *   SP는 공백 한 글자(0x20), CRLF는 "\r\n" 두 글자(0x0D 0x0A)다.
 * buf: '\0'으로 끝나는 문자열이어야 한다. 이 함수는 buf를 바꾸지 않는다.
 * 반환값: 성공하면 0. 호출한 쪽이 free_request(req)를 불러야 한다.
 *         실패하면 -1. req의 세 포인터는 모두 NULL이고 해제할 메모리는 없다.
 * 실패하는 경우:
 *   - buf나 req가 NULL
 *   - CRLF가 없음 (첫 줄이 아직 다 오지 않았거나 형식이 틀림)
 *   - 첫 줄의 공백이 정확히 두 개가 아님 (칸이 세 개가 아님)
 *   - 빈 칸이 있음
 *   - malloc 실패 (메모리 부족)
 */
int parse_request_line(const char *buf, struct http_request *req);

/*
 * parse_request_line이 할당한 세 칸의 메모리를 해제하고 포인터를 NULL로 바꾼다.
 * req가 NULL이거나 이미 해제된 뒤(포인터가 NULL)에 불러도 안전하다.
 */
void free_request(struct http_request *req);

/* read_request_head의 반환값 */
#define HEAD_OK 0         /* "\r\n\r\n"을 찾았다. 헤더를 끝까지 받았다 */
#define HEAD_CLOSED 1     /* "\r\n\r\n"이 오기 전에 클라이언트가 연결을 닫았다 (read가 0을 반환) */
#define HEAD_TOO_LARGE 2  /* 버퍼가 가득 찼는데도 "\r\n\r\n"이 없다 (헤더가 버퍼보다 길다) */
#define HEAD_ERROR (-1)   /* read가 에러로 실패했다 (errno 내용은 stderr에 출력) */

/*
 * 연결 소켓 fd에서 헤더의 끝 "\r\n\r\n"이 나올 때까지 read를 반복해 buf에 이어 붙인다.
 * TCP는 바이트 스트림이라 요청 하나가 read 여러 번에 나뉘어 올 수 있다.
 * 그래서 매번 "이번에 받은 조각"이 아니라 "지금까지 받은 전체"에서 "\r\n\r\n"을 찾는다.
 * buf:  받은 바이트를 담을 버퍼. 함수가 돌아올 때 항상 '\0'으로 끝난다.
 * size: buf의 크기(바이트). 마지막 한 바이트는 '\0' 자리라서 최대 size - 1바이트까지 받는다.
 * len:  지금까지 받은 바이트 수를 써 넣을 곳 ('\0'은 세지 않는다).
 * 반환값: HEAD_OK, HEAD_CLOSED, HEAD_TOO_LARGE, HEAD_ERROR 중 하나.
 *         buf나 len이 NULL이거나 size가 2보다 작으면 HEAD_ERROR.
 * read가 시그널 때문에 중단되면(errno == EINTR) 에러로 보지 않고 다시 read한다.
 */
int read_request_head(int fd, char *buf, size_t size, size_t *len);

/*
 * 요청 대상(target)을 정적 파일 폴더(root) 아래의 파일 경로로 바꿔 path에 쓴다.
 *   path = root + target              예) "./www" + "/img/red.png" → "./www/img/red.png"
 *   target이 '/'로 끝나면 폴더를 가리키는 것으로 보고 "index.html"을 덧붙인다.
 *                                     예) "./www" + "/" → "./www/index.html"
 * 파일이 실제로 있는지는 확인하지 않는다. 문자열만 만든다.
 * path: 결과를 담을 버퍼. size: path의 크기(바이트, 끝의 '\0' 포함).
 * 반환값: 성공하면 0. 결과가 path에 다 들어가지 않으면(잘리면) -1. 이때 path의 내용은 쓰면 안 된다.
 */
int build_file_path(const char *root, const char *target, char *path, size_t size);

/*
 * 요청 대상(target)을 '?' 기준으로 경로와 쿼리 문자열로 나눈다.
 *   예) "/search.html?q=abc" → 경로 "/search.html", 쿼리 문자열 "q=abc"
 *       "/index.html"        → 경로 "/index.html",  쿼리 문자열 "" (빈 문자열)
 * 경로는 path 버퍼(크기 size)에 복사한다. 파일을 찾을 때는 경로만 쓴다.
 * query에는 target 안에서 '?' 다음 글자를 가리키는 포인터를 넣는다 ('?'가 없으면 target 끝의 '\0'을 가리킨다).
 * query는 복사본이 아니라 target의 일부를 가리키므로 target이 해제된 뒤에는 쓰면 안 된다.
 * 반환값: 성공하면 0. 경로가 path에 다 들어가지 않으면 -1.
 */
int split_target(const char *target, char *path, size_t size, const char **query);

/*
 * 경로를 '/'로 나눈 칸 중에 ".."이 있으면 1, 없으면 0을 돌려준다.
 * ".."은 "한 단계 위 폴더"라는 뜻이라서, 그대로 파일 경로에 붙이면 정적 파일 폴더 밖의 파일을 가리킬 수 있다.
 *   예) "/../Makefile" → 1,  "/a/../b" → 1,  "/a..b/c" → 0 (칸 전체가 ".."인 경우만 해당),  "/..." → 0
 */
int path_has_dotdot(const char *path);

/*
 * head(요청 줄과 헤더 전체가 담긴 문자열)에서 이름이 name인 헤더를 찾아 그 값을 value에 복사한다.
 * 헤더 줄의 형식(RFC 9112 5절): 이름 ":" 값 CRLF      예) "Host: localhost:8080\r\n"
 *   - 첫 줄(요청 줄)은 건너뛰고 둘째 줄부터 빈 줄 전까지 본다.
 *   - 이름은 대소문자를 구분하지 않는다 ("content-length"와 "Content-Length"는 같은 헤더).
 *   - 값 앞뒤의 공백과 탭은 떼어 낸다.
 *   - 같은 이름의 헤더가 여러 줄이면 처음 것을 쓴다.
 * 반환값: 찾았으면 0, 그런 헤더가 없으면 1, 값이 value(크기 size)에 다 들어가지 않으면 -1.
 */
int get_header(const char *head, const char *name, char *value, size_t size);

/*
 * Content-Length 헤더의 값(text)을 숫자로 바꿔 *out에 넣는다.
 * 숫자(0~9)로만 이루어져 있어야 한다. 빈 문자열, 부호("-1", "+5"), 다른 글자("12a")가 있으면 실패다.
 * 반환값: 성공하면 0, 형식이 틀리거나 너무 큰 수면 -1.
 */
int parse_content_length(const char *text, size_t *out);

/* read_body의 반환값 */
#define BODY_OK 0       /* content_length바이트를 모두 받았다 */
#define BODY_CLOSED 1   /* 다 받기 전에 클라이언트가 연결을 닫았다 */
#define BODY_ERROR (-1) /* read가 에러로 실패했다 */

/*
 * 요청 본문을 content_length바이트만큼 body에 채운다.
 * 헤더를 읽을 때(read_request_head) 빈 줄 뒤의 본문 일부가 같은 버퍼에 함께 들어왔을 수 있다.
 * 그 부분(already부터 already_len바이트)을 먼저 body에 복사하고, 모자란 만큼만 fd에서 read를 반복해 받는다.
 * 본문에는 끝 표시가 없다. 그래서 "빈 줄을 찾을 때까지"가 아니라 "content_length바이트를 받을 때까지" 읽는다.
 * body: content_length바이트 이상의 크기여야 한다. 이 함수는 끝에 '\0'을 붙이지 않는다.
 * 반환값: BODY_OK, BODY_CLOSED, BODY_ERROR 중 하나.
 */
int read_body(int fd, const char *already, size_t already_len, char *body, size_t content_length);

#endif /* HTTP_H */
