/*
 * 데모용 CGI 프로그램: 3초 동안 기다린 뒤 응답한다.
 * 처리에 오래 걸리는 요청 하나가 다른 클라이언트를 기다리게 하는지 보는 용도 (fork 있음과 없음 비교).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    sleep(3);
    printf("Content-Type: text/plain; charset=utf-8\r\n\r\n");
    printf("slow CGI: 3초 뒤에 응답 (CGI 프로세스 %d)\n", (int)getpid());
    return 0;
}
