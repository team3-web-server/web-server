/*
 * CGI 예제 프로그램 1: 요청 정보를 글자로 돌려준다.
 * 웹 서버 프로세스가 fork + execve로 이 프로그램을 실행한다. 이 프로그램은 소켓을 모른다.
 *   입력: 환경 변수 REQUEST_METHOD(메서드), QUERY_STRING(요청 대상의 '?' 뒤)
 *   출력: 표준 출력(printf)에 "헤더 줄, 빈 줄, 본문" 순서로 쓴다.
 *         웹 서버 프로세스가 파이프로 이 출력을 받아 HTTP 응답으로 만들어 클라이언트에게 보낸다.
 * 호출할 때마다 프로세스 번호와 시각이 달라지므로, 파일을 그대로 보내는 정적 응답과 달리 응답이 매번 새로 만들어진다.
 * 확인: curl 'http://localhost:8080/cgi-bin/hello?name=somang'
 */
#include <stdio.h>     /* printf */
#include <stdlib.h>    /* getenv */
#include <sys/types.h> /* pid_t */
#include <time.h>      /* time, localtime, strftime */
#include <unistd.h>    /* getpid */

int main(void)
{
    /* getenv(이름): 환경 변수의 값을 돌려준다. 그런 변수가 없으면 NULL */
    const char *method = getenv("REQUEST_METHOD");
    const char *query = getenv("QUERY_STRING");

    /* 현재 시각을 "시:분:초" 글자로 만든다 */
    char now[32];
    time_t t = time(NULL);
    strftime(now, sizeof(now), "%H:%M:%S", localtime(&t));

    /* CGI 출력의 앞부분은 헤더다. Content-Type 한 줄을 쓰고 빈 줄로 헤더의 끝을 알린다 */
    printf("Content-Type: text/plain; charset=utf-8\r\n");
    printf("\r\n");

    /* 여기부터가 응답 본문이다 */
    printf("hello CGI\n");
    printf("REQUEST_METHOD=%s\n", method != NULL ? method : "(없음)");
    printf("QUERY_STRING=%s\n", query != NULL ? query : "(없음)");
    printf("CGI 프로세스 번호(pid)=%d\n", (int)getpid());
    printf("시각=%s\n", now);
    return 0;
}
