#include "http.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

/* start부터 len바이트를 새로 할당한 메모리에 복사하고 끝에 '\0'을 붙여 돌려준다 */
static char *copy_field(const char *start, size_t len)
{
    char *field = malloc(len + 1);
    if (field == NULL) {
        return NULL;
    }
    memcpy(field, start, len);
    field[len] = '\0';
    return field;
}

int parse_request_line(const char *buf, struct http_request *req)
{
    if (req == NULL) {
        return -1;
    }

    /* 실패해도 쓰레기 주소가 남지 않게 먼저 NULL로 채운다 */
    req->method = NULL;
    req->target = NULL;
    req->version = NULL;
    if (buf == NULL) {
        return -1;
    }

    /* 첫 줄의 끝(CRLF)을 찾는다 */
    const char *line_end = strstr(buf, "\r\n");
    if (line_end == NULL) {
        return -1;
    }

    /* 공백 두 개를 줄 끝 안에서만 찾는다. 첫 공백 앞이 메서드, 두 공백 사이가 요청 대상, 그 뒤가 버전 */
    const char *sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (sp1 == NULL) {
        return -1;
    }

    const char *target_start = sp1 + 1;
    const char *sp2 = memchr(target_start, ' ', (size_t)(line_end - target_start));
    if (sp2 == NULL) {
        return -1;
    }

    const char *version_start = sp2 + 1;
    /* 공백이 세 개 이상이면 형식이 틀린 것 */
    if (memchr(version_start, ' ', (size_t)(line_end - version_start)) != NULL) {
        return -1;
    }

    size_t method_len = (size_t)(sp1 - buf);
    size_t target_len = (size_t)(sp2 - target_start);
    size_t version_len = (size_t)(line_end - version_start);
    if (method_len == 0 || target_len == 0 || version_len == 0) {
        return -1;
    }

    /* 하나라도 할당에 실패하면 이미 할당한 칸을 해제한다 */
    req->method = copy_field(buf, method_len);
    req->target = copy_field(target_start, target_len);
    req->version = copy_field(version_start, version_len);
    if (req->method == NULL || req->target == NULL || req->version == NULL) {
        free_request(req);
        return -1;
    }
    return 0;
}

void free_request(struct http_request *req)
{
    if (req == NULL) {
        return;
    }

    free(req->method);
    free(req->target);
    free(req->version);

    req->method = NULL;
    req->target = NULL;
    req->version = NULL;
}

int read_request_head(int fd, char *buf, size_t size, size_t *len)
{
    if (buf == NULL || len == NULL || size < 2) {
        return HEAD_ERROR;
    }
    size_t total = 0;
    buf[0] = '\0';
    *len = 0;

    for (;;) {
        /* 남은 자리. 마지막 한 바이트는 '\0' 자리로 남긴다 */
        size_t space = size - 1 - total;
        if (space == 0) {
            return HEAD_TOO_LARGE;
        }

        ssize_t n = read(fd, buf + total, space);
        if (n == -1) {
            if (errno == EINTR) {
                /* 시그널로 중단됐을 뿐이므로 다시 읽는다 */
                continue;
            }
            perror("read");
            return HEAD_ERROR;
        }
        if (n == 0) {
            return HEAD_CLOSED;
        }

        total += (size_t)n;
        buf[total] = '\0';
        *len = total;

        /* 빈 줄이 두 read 사이에 잘려 올 수 있으므로 buf 전체에서 찾는다 */
        if (strstr(buf, "\r\n\r\n") != NULL) {
            return HEAD_OK;
        }
    }
}

#define INDEX_NAME "index.html"

int build_file_path(const char *root, const char *target, char *path, size_t size)
{
    const char *index = "";
    size_t target_len = strlen(target);
    if (target_len > 0 && target[target_len - 1] == '/') {
        index = INDEX_NAME;
    }

    /* snprintf의 반환값이 size 이상이면 결과가 잘린 것 */
    int n = snprintf(path, size, "%s%s%s", root, target, index);
    if (n >= (int)size) {
        return -1;
    }
    return 0;
}

int split_target(const char *target, char *path, size_t size, const char **query)
{
    size_t path_len = strcspn(target, "?");
    if (path_len >= size) {
        return -1;
    }
    memcpy(path, target, path_len);
    path[path_len] = '\0';

    if (target[path_len] == '?') {
        *query = target + path_len + 1;
    } else {
        *query = target + path_len;
    }
    return 0;
}

int path_has_dotdot(const char *path)
{
    const char *p = path;
    while (*p != '\0') {
        if (*p == '/') {
            p++;
            continue;
        }

        /* 칸 하나의 길이: 다음 '/' 또는 문자열 끝까지 */
        size_t seg_len = strcspn(p, "/");
        if (seg_len == 2 && p[0] == '.' && p[1] == '.') {
            return 1;
        }
        p += seg_len;
    }
    return 0;
}

int get_header(const char *head, const char *name, char *value, size_t size)
{
    /* 첫 줄(요청 줄)을 건너뛴다 */
    const char *line = strchr(head, '\n');
    if (line == NULL) {
        return 1;
    }

    return find_header(line + 1, name, value, size);
}

int find_header(const char *lines, const char *name, char *value, size_t size)
{
    size_t name_len = strlen(name);

    const char *line = lines;
    while (*line != '\0') {
        /* 줄 길이에서 끝의 '\r'은 뺀다. 길이가 0이면 빈 줄이므로 헤더가 끝난 것 */
        size_t line_len = strcspn(line, "\n");
        const char *next = line + line_len;
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line_len--;
        }
        if (line_len == 0) {
            break;
        }

        /* 줄이 "name:"으로 시작하면 ':' 뒤의 값에서 앞뒤 공백과 탭을 떼고 복사한다 */
        if (line_len > name_len && line[name_len] == ':' && strncasecmp(line, name, name_len) == 0) {
            const char *v = line + name_len + 1;
            size_t v_len = line_len - name_len - 1;
            while (v_len > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                v_len--;
            }

            while (v_len > 0 && (v[v_len - 1] == ' ' || v[v_len - 1] == '\t')) {
                v_len--;
            }
            if (v_len >= size) {
                return -1;
            }
            memcpy(value, v, v_len);
            value[v_len] = '\0';
            return 0;
        }

        if (*next == '\0') {
            break;
        }
        line = next + 1;
    }
    return 1;
}

int parse_content_length(const char *text, size_t *out)
{
    if (text[0] == '\0') {
        return -1;
    }
    size_t value = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }

        size_t digit = (size_t)(*p - '0');
        /* 더 곱하면 size_t 범위를 넘는다 */
        if (value > (SIZE_MAX - digit) / 10) {
            return -1;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return 0;
}

int read_body(int fd, const char *already, size_t already_len, char *body, size_t content_length)
{
    /* 헤더와 함께 이미 들어온 본문 부분을 먼저 복사한다 */
    size_t total = already_len;
    if (total > content_length) {
        total = content_length;
    }
    memcpy(body, already, total);

    while (total < content_length) {
        ssize_t n = read(fd, body + total, content_length - total);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            return BODY_ERROR;
        }
        if (n == 0) {
            return BODY_CLOSED;
        }
        total += (size_t)n;
    }
    return BODY_OK;
}
