#include "http.h"

#include <errno.h>     /* errno, EINTR */
#include <stddef.h>    /* size_t, NULL */
#include <stdint.h>    /* SIZE_MAX */
#include <stdio.h>     /* perror, snprintf */
#include <stdlib.h>    /* malloc, free */
#include <string.h>    /* strstr, memchr, memcpy, strlen, strcspn, strchr */
#include <strings.h>   /* strncasecmp */
#include <sys/types.h> /* ssize_t */
#include <unistd.h>    /* read */

/*
 * start부터 len바이트를 새로 할당한 메모리에 복사하고 끝에 '\0'을 붙여 그 주소를 돌려준다.
 * malloc(크기): 힙에서 크기 바이트를 할당하고 시작 주소를 돌려준다. 실패하면 NULL.
 * '\0' 자리까지 len + 1바이트를 할당한다.
 * static: 이 파일 안에서만 쓰는 함수라는 뜻
 */
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
    /* 실패로 돌아가더라도 req에 쓰레기 주소가 남지 않게 먼저 NULL로 채운다 */
    req->method = NULL;
    req->target = NULL;
    req->version = NULL;
    if (buf == NULL) {
        return -1;
    }

    /*
     * 1) 첫 줄의 끝을 찾는다.
     * strstr(buf, "\r\n"): buf에서 "\r\n"이 처음 나오는 위치의 포인터를 돌려준다. 없으면 NULL.
     * line_end는 '\r'을 가리키고, 요청 줄은 buf부터 line_end 바로 앞까지다.
     */
    const char *line_end = strstr(buf, "\r\n");
    if (line_end == NULL) {
        return -1;
    }

    /*
     * 2) 첫 번째 공백을 찾는다. 그 앞이 메서드다.
     * memchr(시작, 찾을 글자, 길이): 시작부터 길이 바이트 안에서 글자가 처음 나오는 위치. 없으면 NULL.
     * 길이를 줄 끝까지로 제한해서 다음 줄(헤더)에 있는 공백을 잡지 않게 한다.
     * 포인터끼리 빼면 두 위치 사이의 바이트 수가 나온다.
     */
    const char *sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (sp1 == NULL) {
        return -1;
    }

    /* 3) 두 번째 공백을 찾는다. 첫 번째 공백 다음 글자부터 줄 끝까지에서 찾는다. 두 공백 사이가 요청 대상이다 */
    const char *target_start = sp1 + 1;
    const char *sp2 = memchr(target_start, ' ', (size_t)(line_end - target_start));
    if (sp2 == NULL) {
        return -1;
    }

    /* 4) 두 번째 공백 다음부터 줄 끝까지가 버전이다. 여기에 공백이 또 있으면 칸이 네 개 이상이므로 실패 */
    const char *version_start = sp2 + 1;
    if (memchr(version_start, ' ', (size_t)(line_end - version_start)) != NULL) {
        return -1;
    }

    /* 5) 세 칸의 길이를 구한다. 빈 칸이 있으면 실패. 여기까지는 아직 아무것도 할당하지 않았다 */
    size_t method_len = (size_t)(sp1 - buf);
    size_t target_len = (size_t)(sp2 - target_start);
    size_t version_len = (size_t)(line_end - version_start);
    if (method_len == 0 || target_len == 0 || version_len == 0) {
        return -1;
    }

    /*
     * 6) 세 칸을 각각 할당해 복사한다.
     * 하나라도 할당에 실패하면 이미 할당한 칸을 해제하고 실패를 돌려준다 (메모리 누수 방지).
     * free_request는 NULL인 칸은 건너뛰므로 어디서 실패했든 그대로 부르면 된다.
     */
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
    /* free(NULL)은 아무 일도 하지 않으므로 칸이 NULL이어도 그대로 불러도 된다 */
    free(req->method);
    free(req->target);
    free(req->version);
    /* 해제한 주소를 실수로 다시 쓰거나 두 번 해제하지 않게 NULL로 바꿔 둔다 */
    req->method = NULL;
    req->target = NULL;
    req->version = NULL;
}

int read_request_head(int fd, char *buf, size_t size, size_t *len)
{
    if (buf == NULL || len == NULL || size < 2) {
        return HEAD_ERROR;
    }
    size_t total = 0; /* 지금까지 받은 바이트 수 */
    buf[0] = '\0';
    *len = 0;

    for (;;) {
        /* 남은 자리: 전체 크기에서 이미 받은 만큼과 '\0' 자리 하나를 뺀다 */
        size_t space = size - 1 - total;
        if (space == 0) {
            return HEAD_TOO_LARGE; /* 버퍼가 가득 찼는데 아직 "\r\n\r\n"을 못 찾았다 */
        }

        /* 이미 받은 바이트 바로 뒤(buf + total)부터 이어서 받는다 */
        ssize_t n = read(fd, buf + total, space);
        if (n == -1) {
            if (errno == EINTR) {
                continue; /* 시그널 때문에 중단됐을 뿐이다. 받은 것이 없으니 그대로 다시 read한다 */
            }
            perror("read");
            return HEAD_ERROR;
        }
        if (n == 0) {
            return HEAD_CLOSED; /* 클라이언트가 연결을 닫았다. 더 올 바이트가 없다 */
        }

        total += (size_t)n;
        buf[total] = '\0'; /* read는 '\0'을 붙여 주지 않으므로 받을 때마다 끝에 직접 넣는다 */
        *len = total;

        /* 이번 조각이 아니라 buf 전체에서 찾는다. "\r\n\r\n"이 두 번의 read 사이에서 잘려 올 수 있기 때문이다 */
        if (strstr(buf, "\r\n\r\n") != NULL) {
            return HEAD_OK;
        }
    }
}

/* target이 '/'로 끝날 때 덧붙이는 파일 이름 */
#define INDEX_NAME "index.html"

/* 요청 대상을 파일 경로로 바꾼다. root와 target을 이어 붙이고, target이 '/'로 끝나면 INDEX_NAME을 덧붙인다. */
int build_file_path(const char *root, const char *target, char *path, size_t size)
{
    /* target의 마지막 글자가 '/'인지 본다. target이 빈 문자열이면 마지막 글자가 없으므로 보지 않는다 */
    const char *index = "";
    size_t target_len = strlen(target);
    if (target_len > 0 && target[target_len - 1] == '/') {
        index = INDEX_NAME;
    }

    /*
     * snprintf(버퍼, 크기, 형식, ...)는 printf처럼 글자를 만들되 화면이 아니라 버퍼에 쓴다.
     * 크기를 넘는 부분은 쓰지 않고 잘라 내며(끝에는 항상 '\0'), 잘리지 않았다면 썼을 글자 수를 돌려준다.
     * 그래서 반환값이 size 이상이면 결과가 잘렸다는 뜻이다.
     */
    int n = snprintf(path, size, "%s%s%s", root, target, index);
    if (n >= (int)size) {
        return -1;
    }
    return 0;
}

int split_target(const char *target, char *path, size_t size, const char **query)
{
    /* strcspn(문자열, "?"): 문자열 처음부터 '?'가 나오기 전까지의 글자 수. '?'가 없으면 문자열 전체 길이 */
    size_t path_len = strcspn(target, "?");
    if (path_len >= size) {
        return -1; /* 경로와 끝의 '\0'이 버퍼에 들어가지 않는다 */
    }
    memcpy(path, target, path_len);
    path[path_len] = '\0';

    /* target[path_len]은 '?'이거나 문자열 끝('\0')이다. '?'이면 그 다음 글자부터가 쿼리 문자열이다 */
    if (target[path_len] == '?') {
        *query = target + path_len + 1;
    } else {
        *query = target + path_len;
    }
    return 0;
}

int path_has_dotdot(const char *path)
{
    /* p는 지금 보고 있는 칸의 첫 글자를 가리킨다. '/'를 만날 때마다 다음 칸으로 넘어간다 */
    const char *p = path;
    while (*p != '\0') {
        if (*p == '/') {
            p++;
            continue;
        }
        /* 칸의 길이: 다음 '/' 또는 문자열 끝까지의 글자 수 */
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
    size_t name_len = strlen(name);

    /* 첫 줄(요청 줄)을 건너뛴다. strchr(문자열, 글자)는 글자가 처음 나오는 위치를 돌려준다. 없으면 NULL */
    const char *line = strchr(head, '\n');
    if (line == NULL) {
        return 1;
    }
    line++; /* '\n' 다음 글자가 둘째 줄의 시작이다 */

    /* 한 줄씩 본다. line은 지금 보는 줄의 첫 글자를 가리킨다 */
    while (*line != '\0') {
        /* 이 줄의 길이: '\n' 전까지의 글자 수. 줄 끝에 '\r'이 있으면 그것도 뺀다 */
        size_t line_len = strcspn(line, "\n");
        const char *next = line + line_len; /* '\n' 또는 문자열 끝 */
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line_len--;
        }
        if (line_len == 0) {
            break; /* 빈 줄이다. 헤더가 끝났다 */
        }

        /*
         * 이 줄이 "name:"으로 시작하는지 본다.
         * strncasecmp(a, b, n): 앞의 n글자를 대소문자 구분 없이 비교한다 (같으면 0).
         */
        if (line_len > name_len && line[name_len] == ':' && strncasecmp(line, name, name_len) == 0) {
            /* 값은 ':' 다음부터 줄 끝까지다. 앞쪽의 공백과 탭을 건너뛴다 */
            const char *v = line + name_len + 1;
            size_t v_len = line_len - name_len - 1;
            while (v_len > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                v_len--;
            }
            /* 뒤쪽의 공백과 탭도 뗀다 */
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

        /* 다음 줄로 넘어간다 */
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
        return -1; /* 빈 문자열 */
    }
    size_t value = 0;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1; /* 숫자가 아닌 글자 */
        }
        /* 글자 '0'~'9'에서 '0'을 빼면 숫자 0~9가 된다. 지금까지의 값에 10을 곱하고 더한다 */
        size_t digit = (size_t)(*p - '0');
        if (value > (SIZE_MAX - digit) / 10) {
            return -1; /* 더 곱하면 size_t가 담을 수 있는 범위를 넘는다 */
        }
        value = value * 10 + digit;
    }
    *out = value;
    return 0;
}

int read_body(int fd, const char *already, size_t already_len, char *body, size_t content_length)
{
    /* 헤더와 함께 이미 들어온 본문 부분을 먼저 복사한다. content_length보다 많이 들어왔으면 그만큼만 쓴다 */
    size_t total = already_len;
    if (total > content_length) {
        total = content_length;
    }
    memcpy(body, already, total);

    /* 모자란 만큼을 read로 받는다. 한 번에 다 오지 않을 수 있으므로 total이 content_length가 될 때까지 반복한다 */
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
