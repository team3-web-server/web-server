#ifndef HTTP_H
#define HTTP_H

/*
 * HTTP 요청을 읽고 해석하는 함수 모음.
 * 단계 2: 요청의 첫 줄(요청 줄)을 세 칸으로 나누는 함수.
 * 단계 3: 연결 소켓에서 헤더의 끝(빈 줄, "\r\n\r\n")까지 읽는 함수.
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

#endif /* HTTP_H */
