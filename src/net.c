#include "net.h"

#include <errno.h>      /* errno */
#include <netinet/in.h> /* struct sockaddr_in, htons, htonl, INADDR_ANY */
#include <stdio.h>      /* perror */
#include <string.h>     /* memset */
#include <sys/socket.h> /* socket, bind, AF_INET, SOCK_STREAM */

/* TCP 소켓을 하나 만들어서 그 fd 번호를 돌려주는 함수. 만들지 못하면 에러 원인을 출력하고 -1을 돌려준다. */
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
        int saved_errno = errno; /* perror가 errno를 덮어쓸 수 있으므로 저장했다가 되돌린다 (bind_port 참고) */
        perror("socket");
        errno = saved_errno;
        return -1;
    }
    return fd;
}

/* 소켓에 IP 주소와 포트를 붙이는 함수. 성공하면 0, 실패하면 에러 원인을 출력하고 -1을 돌려준다. */
int bind_port(int fd, int port)
{
    /* 포트와 IP 주소 값을 담는 구조체 변수를 선언한다. */
    struct sockaddr_in addr;

    /* 쓰지 않는 칸(sin_zero 등)에 쓰레기 값이 남지 않도록 구조체 전체를 0으로 채운다 */
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET; /* 주소 체계(형식) 표시, 이 구조체는 IPv4 주소 라는 것을 알린다. */

    /*
     * 네트워크는 큰 쪽 바이트부터 보내는 빅엔디언(네트워크 바이트 순서)을 쓴다.
     * x86 CPU는 리틀엔디언이라 그대로 넣으면 바이트가 뒤집힌다.
     * 예) 8080 = 0x1F90 → 메모리에 1F 90 순서로 들어가야 한다.
     *
     * htons = host to network short, 호스트 순서의 16비트 값을 네트워크 순서로 바꾼다.
     */
    addr.sin_port = htons((unsigned short)port);

    /* htonl = host to network long, 호스트(내 CPU) 바이트 순서의 32비트 값을 네트워크 바이트 순서로 바꾼다. */
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* 0.0.0.0: 이 컴퓨터의 모든 IPv4 주소로 오는 연결을 받는다 */

    /*
     * bind()는 범용 주소 타입(struct sockaddr *)을 받으므로 IPv4 구조체를 형변환해서 넘기고,
     * 구조체 크기를 함께 알려 준다.
     */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        /*
         * perror()는 출력하는 과정에서 errno를 덮어쓸 수 있다(glibc에서 실제로 EINVAL로 바뀌는 것을 확인함).
         * 호출한 쪽이 errno로 실패 원인(EADDRINUSE 등)을 구분할 수 있게, 출력 전에 저장했다가 되돌린다.
         */
        int saved_errno = errno;
        perror("bind");
        errno = saved_errno;
        return -1;
    }
    return 0;
}
