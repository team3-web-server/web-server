/*
 * 웹 서버 진입점.
 * 단계 1-1: TCP 소켓을 하나 만들고, 받은 fd 번호를 출력한 뒤 닫는다.
 * bind/listen/accept는 다음 조각에서 추가한다.
 */
#include "net.h"

#include <stdio.h>  /* printf, perror */
#include <unistd.h> /* close */

int main(void)
{
    int fd = create_tcp_socket();
    if (fd == -1) {
        return 1;
    }
    printf("socket fd = %d\n", fd);

    /* fd를 닫아야 커널이 소켓 객체를 해제하고 fd 테이블의 칸이 비워진다 */
    if (close(fd) == -1) {
        perror("close");
        return 1;
    }
    return 0;
}
