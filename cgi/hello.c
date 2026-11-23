/*
 * CGI 예제 1: 환경 변수로 받은 요청 정보를 돌려준다.
 * 확인: curl 'http://localhost:8080/cgi-bin/hello?name=somang'
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    const char *method = getenv("REQUEST_METHOD");
    const char *query = getenv("QUERY_STRING");

    char now[32];
    time_t t = time(NULL);
    strftime(now, sizeof(now), "%H:%M:%S", localtime(&t));

    /* CGI 출력은 "헤더 줄, 빈 줄, 본문" 순서로 쓴다 */
    printf("Content-Type: text/plain; charset=utf-8\r\n");
    printf("\r\n");

    printf("hello CGI\n");
    printf("REQUEST_METHOD=%s\n", method != NULL ? method : "(없음)");
    printf("QUERY_STRING=%s\n", query != NULL ? query : "(없음)");
    printf("CGI 프로세스 번호(pid)=%d\n", (int)getpid());
    printf("시각=%s\n", now);
    return 0;
}
