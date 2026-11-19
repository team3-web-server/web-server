#include "net.h"

#include <stdio.h>      /* perror */
#include <sys/socket.h> /* socket, AF_INET, SOCK_STREAM */

int create_tcp_socket(void)
{
    /*
     * AF_INET    : 주소 체계는 IPv4
     * SOCK_STREAM: 연결을 맺고 바이트를 순서대로 전달하는 스트림 소켓 (= TCP)
     * 0          : 위 두 값에 맞는 기본 프로토콜(TCP)을 커널이 고른다
     */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        /* socket()은 실패하면 -1을 반환하고 원인을 전역 변수 errno에 남긴다 */
        perror("socket");
        return -1;
    }
    return fd;
}
