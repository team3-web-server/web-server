#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define MAX_URI 2048
#define MAX_METHOD 16
#define MAX_VERSION 16
#define MAX_HEADERS 100
#define MAX_HEADER_BYTES 8192
#define MAX_BODY (1024 * 1024)
#define NO_CONTENT_LENGTH (-1)

enum http_method { HTTP_UNKNOWN, HTTP_GET, HTTP_HEAD, HTTP_POST };

struct request {
    enum http_method method;
    char method_name[MAX_METHOD];
    char uri[MAX_URI];
    char version[MAX_VERSION];
    char content_type[256];
    long content_length;
    int chunked;
    char *body;
    size_t body_len;
};

/* 소켓 쓰기. EPIPE를 포함한 실패면 -1. */
int http_write_all(int fd, const void *buf, size_t n);

/* 오류 응답을 보낸다. head_only면 본문을 생략한다. */
int send_error(int fd, int code, const char *cause, const char *detail, int head_only);

/* 요청 하나를 읽어 req를 채운다. 0=성공, -1=EOF, 양수=HTTP 오류 코드.
 * 성공 시 req->body는 호출자가 free한다. 오류 시 read_request가 이미 해제한다. */
int read_request(int fd, struct request *req);

#endif
