/*
 * QUERY_STRING "a&b"의 두 수를 더해 HTML로 출력하는 CGI 프로그램 (CS:APP 그림 11.27)
 * Tiny가 표준 출력을 클라이언트 연결로 바꿔 둔 뒤 실행하므로, 출력이 곧 HTTP 응답의 나머지다.
 */
#include "netlib.h"

int main(void)
{
    int a = 0, b = 0;
    char *query = getenv("QUERY_STRING");

    if (query != NULL) {
        char *amp = strchr(query, '&');
        *amp = '\0'; /* 책 그대로: '&'가 없으면 여기서 크래시 (README의 한계 참고) */
        a = atoi(query);
        b = atoi(amp + 1);
    }

    char content[MAXLINE];
    snprintf(content, sizeof(content),
             "Welcome to add.com: THE Internet addition portal.\r\n<p>"
             "The answer is: %d + %d = %d\r\n<p>"
             "Thanks for visiting!\r\n",
             a, b, a + b);

    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
    exit(0);
}
