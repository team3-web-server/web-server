#include "http.h"

#include <errno.h>     /* errno, EINTR */
#include <stddef.h>    /* size_t, NULL */
#include <stdio.h>     /* perror, snprintf */
#include <stdlib.h>    /* malloc, free */
#include <string.h>    /* strstr, memchr, memcpy, strlen */
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
