#include "http.h"

#include <stddef.h> /* size_t, NULL */
#include <stdlib.h> /* malloc, free */
#include <string.h> /* strstr, memchr, memcpy */

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
