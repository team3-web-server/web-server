/*
 * CGI 예제 프로그램 2: POST 본문을 그대로 돌려준다.
 *   입력: 환경 변수 CONTENT_LENGTH(본문 바이트 수), 표준 입력(본문).
 *         웹 서버 프로세스가 파이프로 본문을 이 프로그램의 표준 입력에 넣어 준다.
 *   출력: 표준 출력에 "헤더 줄, 빈 줄, 본문" 순서로 쓴다.
 * 확인: curl -d 'msg=hello' http://localhost:8080/cgi-bin/echo
 */
#include <stdio.h>  /* printf, fread, fwrite, stdin, stdout */
#include <stdlib.h> /* getenv, strtol, malloc, free */

int main(void)
{
    /* 본문 길이를 환경 변수에서 읽는다. 없으면(GET 요청) 0으로 본다 */
    const char *text = getenv("CONTENT_LENGTH");
    long length = 0;
    if (text != NULL) {
        length = strtol(text, NULL, 10);
    }
    if (length < 0) {
        return 1; /* 0이 아닌 값으로 끝나면 웹 서버 프로세스가 500을 보낸다 */
    }

    /* 본문을 담을 메모리를 할당하고, 표준 입력에서 length바이트를 읽는다 */
    char *body = malloc((size_t)length + 1);
    if (body == NULL) {
        return 1;
    }
    size_t got = fread(body, 1, (size_t)length, stdin);

    printf("Content-Type: text/plain; charset=utf-8\r\n");
    printf("\r\n");
    printf("CONTENT_LENGTH=%ld\n", length);
    printf("받은 본문 %zu바이트:\n", got);
    fwrite(body, 1, got, stdout); /* 본문은 글자가 아닐 수도 있으므로 바이트 수를 정해 그대로 쓴다 */
    printf("\n");

    free(body);
    return 0;
}
