#ifndef HTTP_H
#define HTTP_H

/*
 * HTTP 요청을 해석하는 함수 모음.
 * 단계 2: 요청의 첫 줄(요청 줄)을 세 칸으로 나누는 함수만 있다.
 */

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

#endif /* HTTP_H */
