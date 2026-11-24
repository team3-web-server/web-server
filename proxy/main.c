/*
 * Reverse Proxy 진입점.
 * 실행: ./build/proxy [포트] [설정 파일]   (기본값: 8080, proxy.conf)
 * 흐름: 설정 파일을 읽는다 → socket → bind → listen 뒤, [accept → 새 스레드에게 연결을 맡김]을 반복한다.
 * 연결마다 스레드를 하나 만든다. 새 스레드가 proxy_connection → close를 하고 끝난다.
 * 그래서 처리에 오래 걸리는 연결이 있어도 main 스레드는 바로 다음 accept로 돌아간다.
 */
#include "net.h"
#include "proxy.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define DEFAULT_CONFIG "proxy.conf"

/* 새 스레드에게 넘기는 값. pthread_create는 포인터 하나만 넘길 수 있어서 한 구조체에 담는다 */
struct client_arg {
    int client_fd;
    struct proxy_config *cfg;
};

/* 새 스레드가 실행하는 함수: 연결 하나를 처리하고 닫는다 */
static void *handle_client(void *arg)
{
    struct client_arg *client = arg;
    int client_fd = client->client_fd;
    struct proxy_config *cfg = client->cfg;
    /* main 스레드가 malloc한 칸. 값을 꺼냈으니 여기서 해제한다 */
    free(client);

    proxy_connection(client_fd, cfg);
    close(client_fd);
    return NULL;
}

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

        /*
         * client_fd를 새 스레드에 넘긴다. 지역 변수 client_fd의 주소를 넘기면 안 된다:
         * 새 스레드가 읽기 전에 main 스레드가 다음 accept로 그 변수를 덮어쓸 수 있다. 연결마다 칸을 새로 만든다.
         */
        struct client_arg *client = malloc(sizeof(*client));
        if (client == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }
        client->client_fd = client_fd;
        client->cfg = &cfg;

        pthread_t tid;
        int err = pthread_create(&tid, NULL, handle_client, client);
        if (err != 0) {
            /* pthread 함수는 errno를 쓰지 않고 에러 번호를 반환한다 */
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            free(client);
            close(client_fd);
            continue;
        }
        /* 끝난 스레드를 기다리는(pthread_join) 스레드가 없으므로, 끝나면 스스로 정리되게 한다 */
        pthread_detach(tid);
    }
}
