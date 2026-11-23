#include "net.h"

#include <arpa/inet.h>  /* inet_ntop */
#include <errno.h>      /* errno, EINTR */
#include <netinet/in.h> /* struct sockaddr_in, htons, htonl, ntohs, INADDR_ANY, INET_ADDRSTRLEN */
#include <stdio.h>      /* perror, printf */
#include <string.h>     /* memset */
#include <sys/socket.h> /* socket, setsockopt, bind, listen, accept, AF_INET, SOCK_STREAM, socklen_t */
#include <sys/types.h>  /* ssize_t */
#include <unistd.h>     /* write */

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

/* 서버를 바로 다시 실행해도 bind가 되도록 SO_REUSEADDR 옵션을 켜는 함수. 성공하면 0, 실패하면 -1을 돌려준다. */
int set_reuseaddr(int fd)
{
    int on = 1; /* 옵션 값: 1은 켠다, 0은 끈다 */

    /*
     * setsockopt(소켓 fd, 옵션이 속한 층, 옵션 이름, 옵션 값의 주소, 옵션 값의 크기)
     * SOL_SOCKET: 소켓 자체에 대한 옵션이라는 뜻 (TCP나 IP 층의 옵션이 아님)
     * SO_REUSEADDR: TIME_WAIT 상태의 연결이 남아 있는 주소에도 bind를 허용한다
     */

    /* setsockopt은 소켓의 옵션 값을 바꾸는 시스템 콜이다. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        int saved_errno = errno;
        perror("setsockopt(SO_REUSEADDR)");
        errno = saved_errno;
        return -1;
    }
    return 0;
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

/* bind된 소켓을 듣기 소켓으로 바꾸는 함수. 성공하면 0, 실패하면 에러 원인을 출력하고 -1을 돌려준다. */
int listen_port(int fd, int backlog)
{
    /*
     * listen(소켓 fd, 대기열 최대 길이)
     * 커널이 이 소켓을 듣기 상태(LISTEN)로 바꾸고 연결 대기열을 만든다.
     * 이후 클라이언트의 연결 요청은 커널이 받아서 handshake까지 끝내고 대기열에 넣어 둔다.
     * backlog가 시스템 상한(net.core.somaxconn)보다 크면 커널이 상한으로 줄인다.
     */
    if (listen(fd, backlog) == -1) {
        int saved_errno = errno; /* perror가 errno를 덮어쓸 수 있어서 저장해 둔다 */
        perror("listen");
        errno = saved_errno;
        return -1;
    }
    return 0;
}

/* 대기열에서 연결 하나를 꺼내 그 연결의 새 fd를 돌려주는 함수. 실패하면 에러 원인을 출력하고 -1을 돌려준다. */
int accept_client(int fd)
{
    /* 커널이 클라이언트의 주소(IP, 포트)를 적어 줄 구조체 변수 */
    struct sockaddr_in client;

    /*
     * 값-결과 인자: 부를 때는 "구조체 크기가 이만큼"이라고 알려 주고,
     * 돌아올 때는 커널이 "실제로 이만큼 적었다"로 바꿔 놓는다. 그래서 주소(&len)로 넘긴다.
     */
    socklen_t len = sizeof(client);

    /*
     * accept(듣기 소켓 fd, 클라이언트 주소를 받을 곳, 그 크기의 주소)
     * 대기열에 완료된 연결이 없으면 여기서 멈춰 기다린다.
     * 돌려주는 값은 듣기 소켓과 다른 새 fd(연결 소켓)이다.
     */
    int conn_fd = accept(fd, (struct sockaddr *)&client, &len);
    if (conn_fd == -1) {
        int saved_errno = errno;
        perror("accept");
        errno = saved_errno;
        return -1;
    }

    /*
     * 주소를 사람이 읽을 수 있게 바꿔 출력한다.
     * 커널이 적어 준 주소는 네트워크 바이트 순서의 숫자다.
     * inet_ntop: 32비트 IPv4 주소 숫자 → "127.0.0.1" 같은 문자열 (network to presentation)
     * ntohs: 네트워크 순서의 16비트 포트 → 호스트 순서 (network to host short)
     */
    char ip[INET_ADDRSTRLEN]; /* IPv4 문자열이 들어갈 최대 길이(16) */
    if (inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip)) != NULL) {
        printf("연결 수락: 클라이언트 %s:%d (conn fd = %d)\n", ip, ntohs(client.sin_port), conn_fd);
    }
    return conn_fd;
}

/* buf의 len바이트를 fd에 전부 쓴다. write가 일부만 쓰고 돌아오면 남은 부분을 이어서 다시 쓴다. */
int write_all(int fd, const char *buf, size_t len)
{
    size_t left = len; /* 아직 쓰지 못한 바이트 수 */

    while (left > 0) {
        /*
         * write(fd, 시작 주소, 바이트 수)는 프로세스의 바이트를 커널의 송신 버퍼로 복사하고
         * 복사한 바이트 수를 돌려준다. 요청한 수보다 작을 수 있다.
         */
        ssize_t n = write(fd, buf, left);
        if (n == -1) {
            if (errno == EINTR) {
                continue; /* 시그널 때문에 중단됐을 뿐이다. 쓴 것이 없으니 그대로 다시 write한다 */
            }
            int saved_errno = errno;
            perror("write");
            errno = saved_errno;
            return -1;
        }
        buf += n;          /* 쓴 만큼 시작 위치를 앞으로 옮긴다 */
        left -= (size_t)n; /* 쓴 만큼 남은 수를 줄인다 */
    }
    return 0;
}
