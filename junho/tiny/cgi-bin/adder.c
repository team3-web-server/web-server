/*
 * adder.c - a minimal CGI program that adds two numbers together
 * (CS:APP 그림 11.27)
 */
#include "csapp.h"

int main(void) {
    char *buf, *p;
    char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE];
    int n1=0, n2=0;

    /* Extract the two arguments */
    if ((buf = getenv("QUERY_STRING")) != NULL) {
        p = strchr(buf, '&');
        *p = '\0';
        strcpy(arg1, buf);
        strcpy(arg2, p+1);
        n1 = atoi(arg1);
        n2 = atoi(arg2);
    }

    /* Make the response body */
    /* 책 원문 sprintf(content, "%s...", content)는 원본과 대상이 겹쳐 정의되지 않은 동작이다.
     * -O2(_FORTIFY_SOURCE)에서는 앞 내용이 지워지는 것을 확인해, 끝 위치(content + strlen(content))에 이어 쓰도록 바꿨다. */
    sprintf(content, "QUERY_STRING=%s", buf);
    sprintf(content, "Welcome to add.com: ");
    sprintf(content + strlen(content), "THE Internet addition portal.\r\n<p>");
    sprintf(content + strlen(content), "The answer is: %d + %d = %d\r\n<p>",
            n1, n2, n1 + n2);
    sprintf(content + strlen(content), "Thanks for visiting!\r\n");

    /* Generate the HTTP response */
    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);

    exit(0);
}
