/*
 * HTTP 요청 파싱과 오류 응답.
 * 연결에서 요청 줄·헤더·POST 본문을 읽어 struct request를 채운다.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "http.h"

#include "netlib.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* EPIPE를 포함한 쓰기 실패를 호출자에게 알린다. SIGPIPE는 main에서 무시한다. */
int http_write_all(int fd, const void *buf, size_t n)
{
    return rio_writen(fd, buf, n) == (ssize_t)n ? 0 : -1;
}

static const char *reason_phrase(int code)
{
    switch (code) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Entity";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default: return "Error";
    }
}

int send_error(int fd, int code, const char *cause, const char *detail,
               int head_only)
{
    char body[1024], header[512];
    const char *reason = reason_phrase(code);

    snprintf(body, sizeof(body),
             "<html><title>Tiny Error</title>"
             "<body bgcolor=ffffff>\r\n"
             "%d: %s\r\n"
             "<p>%s: %s\r\n"
             "<hr><em>The Tiny Web server</em>\r\n",
             code, reason, detail, cause);
    size_t body_len = strlen(body);
    snprintf(header, sizeof(header),
             "HTTP/1.0 %d %s\r\n"
             "Server: Tiny Web Server\r\n"
             "Connection: close\r\n"
             "Content-type: text/html\r\n"
             "Content-length: %zu\r\n\r\n",
             code, reason, body_len);

    if (http_write_all(fd, header, strlen(header)) < 0) {
        return -1;
    }
    if (!head_only && http_write_all(fd, body, body_len) < 0) {
        return -1;
    }
    return 0;
}

static int parse_method(const char *name, enum http_method *out)
{
    static const struct {
        const char *name;
        enum http_method method;
    } methods[] = {
        {"GET", HTTP_GET},
        {"HEAD", HTTP_HEAD},
        {"POST", HTTP_POST},
    };

    for (size_t i = 0; i < ARRAY_LEN(methods); i++) {
        if (strcasecmp(name, methods[i].name) == 0) {
            *out = methods[i].method;
            return 0;
        }
    }
    return -1;
}

static int is_supported_version(const char *version)
{
    return strcmp(version, "HTTP/1.0") == 0 || strcmp(version, "HTTP/1.1") == 0;
}

/* 요청 줄 "METHOD URI VERSION"을 읽는다. 0=성공, -1=EOF, 양수=오류 코드. */
static int read_request_line(rio_t *rp, struct request *req)
{
    char line[MAXLINE];

    line[0] = '\0';
    ssize_t n = rio_readlineb(rp, line, sizeof(line));
    if (n <= 0) {
        return -1;
    }
    if (line[n - 1] != '\n') {
        return 414; /* 개행 없이 버퍼가 찼다 */
    }
    line[strcspn(line, "\r\n")] = '\0';

    char *method = line;
    char *method_end = strchr(method, ' ');
    if (method_end == NULL) {
        return 400;
    }
    char *uri = method_end + 1;
    char *uri_end = strchr(uri, ' ');
    if (uri_end == NULL) {
        return 400;
    }
    char *version = uri_end + 1;
    if (strchr(version, ' ') != NULL) {
        return 400;
    }
    *method_end = '\0';
    *uri_end = '\0';

    if (*method == '\0' || *uri == '\0' || *version == '\0') {
        return 400;
    }
    if (strlen(method) >= sizeof(req->method_name)) {
        return 400;
    }
    if (strlen(uri) >= sizeof(req->uri)) {
        return 414;
    }
    if (strlen(version) >= sizeof(req->version)) {
        return 505;
    }
    strcpy(req->method_name, method);
    strcpy(req->uri, uri);
    strcpy(req->version, version);

    if (parse_method(req->method_name, &req->method) < 0) {
        return 501;
    }
    if (!is_supported_version(req->version)) {
        return 505;
    }

    printf("Request line: %s %s %s\n", req->method_name, req->uri, req->version);
    return 0;
}

/* s의 앞뒤에서 chars에 든 문자를 잘라 낸다. */
static char *trim(char *s, const char *chars)
{
    s += strspn(s, chars);
    char *end = s + strlen(s);
    while (end > s && strchr(chars, end[-1]) != NULL) {
        *--end = '\0';
    }
    return s;
}

/* 10진수 숫자만 받는다. 부호·공백·오버플로는 -1. */
static int parse_content_length(const char *value, long *out)
{
    if (*value == '\0') {
        return -1;
    }
    long parsed = 0;
    for (const char *p = value; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
        int digit = *p - '0';
        if (parsed > (LONG_MAX - digit) / 10) {
            return -1;
        }
        parsed = parsed * 10 + digit;
    }
    *out = parsed;
    return 0;
}

/* 서버가 쓰는 헤더만 req에 저장한다. 0=성공, 양수=오류 코드. */
static int store_header(struct request *req, const char *name, const char *value)
{
    if (strcasecmp(name, "Content-Length") == 0) {
        if (req->content_length != NO_CONTENT_LENGTH) {
            return 400; /* 중복 Content-Length */
        }
        return parse_content_length(value, &req->content_length) < 0 ? 400 : 0;
    }
    if (strcasecmp(name, "Content-Type") == 0) {
        snprintf(req->content_type, sizeof(req->content_type), "%s", value);
    } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
        req->chunked = 1;
    }
    return 0;
}

/* 헤더를 빈 줄까지 읽는다. 0=성공, 양수=오류 코드. */
static int read_requesthdrs(rio_t *rp, struct request *req)
{
    char line[MAXLINE];
    size_t total = 0;

    for (int count = 0; count < MAX_HEADERS; count++) {
        line[0] = '\0';
        ssize_t n = rio_readlineb(rp, line, sizeof(line));
        if (n <= 0) {
            return 400; /* 헤더가 빈 줄로 끝나지 않았다 */
        }
        if (line[n - 1] != '\n') {
            return 431;
        }
        total += (size_t)n;
        if (total > MAX_HEADER_BYTES) {
            return 431;
        }
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            return 0;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            return 400; /* obs-fold는 받지 않는다 */
        }

        printf("Request header: %s", line);

        char *colon = strchr(line, ':');
        if (colon == NULL) {
            return 400;
        }
        *colon = '\0';
        char *name = trim(line, " \t");
        char *value = trim(colon + 1, " \t\r\n");

        int rc = store_header(req, name, value);
        if (rc > 0) {
            return rc;
        }
    }
    return 431;
}

/* Content-Length만큼 본문을 읽는다. 0=성공, 양수=오류 코드. */
static int read_body(rio_t *rp, struct request *req)
{
    if (req->content_length == NO_CONTENT_LENGTH) {
        return 400;
    }
    if (req->content_length > MAX_BODY) {
        return 413;
    }
    req->body_len = (size_t)req->content_length;
    req->body = malloc(req->body_len > 0 ? req->body_len : 1);
    if (req->body == NULL) {
        return 500;
    }
    if (req->body_len > 0 &&
        rio_readnb(rp, req->body, req->body_len) != (ssize_t)req->body_len) {
        free(req->body);
        req->body = NULL;
        return 400;
    }
    return 0;
}

int read_request(int fd, struct request *req)
{
    memset(req, 0, sizeof(*req));
    req->content_length = NO_CONTENT_LENGTH;

    rio_t rio;
    rio_readinitb(&rio, fd);

    int rc = read_request_line(&rio, req);
    if (rc != 0) {
        return rc;
    }
    rc = read_requesthdrs(&rio, req);
    if (rc > 0) {
        return rc;
    }
    if (req->chunked) {
        return 501;
    }
    if (req->method == HTTP_POST) {
        return read_body(&rio, req);
    }
    return 0;
}
