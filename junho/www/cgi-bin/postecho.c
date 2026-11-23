/*
 * POST 본문을 그대로 돌려주는 CGI.
 * 서버가 Content-Length만큼 표준 입력으로 넘겨 준 본문을 읽어 text/plain으로 출력한다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BODY (1024 * 1024)

int main(void)
{
    const char *content_length = getenv("CONTENT_LENGTH");
    long n = 0;

    if (content_length != NULL && content_length[0] != '\0') {
        char *end = NULL;
        n = strtol(content_length, &end, 10);
        if (*end != '\0' || n < 0 || n > MAX_BODY) {
            fprintf(stdout, "Content-type: text/plain\r\n\r\ninvalid CONTENT_LENGTH\n");
            return 0;
        }
    }

    char *body = malloc((size_t)n + 1);
    if (body == NULL) {
        fprintf(stdout, "Content-type: text/plain\r\n\r\nout of memory\n");
        return 0;
    }
    size_t got = fread(body, 1, (size_t)n, stdin);
    body[got] = '\0';

    printf("Content-type: text/plain\r\n");
    printf("Content-length: %zu\r\n\r\n", got);
    fwrite(body, 1, got, stdout);
    fflush(stdout);
    free(body);
    return 0;
}
