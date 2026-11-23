/*
 * CGI 예제 2: 표준 입력으로 받은 POST 본문을 그대로 돌려준다.
 * 확인: curl -d 'msg=hello' http://localhost:8080/cgi-bin/echo
 */
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const char *text = getenv("CONTENT_LENGTH");
    long length = 0;
    if (text != NULL) {
        length = strtol(text, NULL, 10);
    }
    /* 0이 아닌 값으로 끝나면 웹 서버 프로세스가 500을 보낸다 */
    if (length < 0) {
        return 1;
    }

    char *body = malloc((size_t)length + 1);
    if (body == NULL) {
        return 1;
    }
    size_t got = fread(body, 1, (size_t)length, stdin);

    printf("Content-Type: text/plain; charset=utf-8\r\n");
    printf("\r\n");
    printf("CONTENT_LENGTH=%ld\n", length);
    printf("받은 본문 %zu바이트:\n", got);
    fwrite(body, 1, got, stdout);
    printf("\n");

    free(body);
    return 0;
}
