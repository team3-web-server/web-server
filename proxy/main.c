/*
 * Reverse Proxy 진입점.
 * 실행: ./build/proxy [포트] [설정 파일]   (기본값: 8080, proxy.conf)
 * 흐름: 설정 파일을 읽는다 → socket → bind → listen 뒤, [accept → proxy_connection → close]를 반복한다.
 * 한 번에 연결 하나씩 처리한다.
 */
#include "net.h"
#include "proxy.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define DEFAULT_CONFIG "proxy.conf"

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 클라이언트나 백엔드가 먼저 닫은 소켓에 write해도 프로세스가 끝나지 않게 한다 */
    signal(SIGPIPE, SIG_IGN);

    if (argc > 3) {
        fprintf(stderr, "사용법: %s [포트] [설정 파일]\n", argv[0]);
        return 1;
    }
    int port = DEFAULT_PORT;
    const char *config_path = DEFAULT_CONFIG;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port < 1 || port > 65535) {
            fprintf(stderr, "포트는 1~65535 사이의 숫자여야 합니다: %s\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3) {
        config_path = argv[2];
    }

    struct proxy_config cfg;
    if (load_config(config_path, &cfg) == -1) {
        return 1;
    }
    for (int i = 0; i < cfg.route_count; i++) {
        printf("규칙 %d: %s →", i + 1, cfg.routes[i].prefix);
        for (int j = 0; j < cfg.routes[i].backend_count; j++) {
            printf(" %s:%d", cfg.routes[i].backends[j].host, cfg.routes[i].backends[j].port);
        }
        printf("\n");
    }

    int fd = create_tcp_socket();
    if (fd == -1) {
        return 1;
    }
    if (set_reuseaddr(fd) == -1 || bind_port(fd, port) == -1 || listen_port(fd, SOMAXCONN) == -1) {
        close(fd);
        return 1;
    }
    printf("프록시 listen 중: 포트 %d, 설정 파일 %s (끝내려면 Ctrl+C)\n", port, config_path);

    for (;;) {
        int client_fd = accept_client(fd);
        if (client_fd == -1) {
            continue;
        }
        proxy_connection(client_fd, &cfg);
        close(client_fd);
    }
}
