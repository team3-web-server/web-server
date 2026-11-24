#ifndef PROXY_H
#define PROXY_H

/*
 * Reverse Proxy: 클라이언트의 요청을 받아 백엔드 서버 중 하나에 그대로 전달하고, 백엔드의 응답을 클라이언트에게 돌려준다.
 * 어느 백엔드로 보낼지는 요청 경로의 앞부분(접두사)으로 정한다.
 */

#define MAX_ROUTES 8
#define MAX_BACKENDS 8
#define PREFIX_SIZE 64
#define HOST_SIZE 64

/* 백엔드 서버 하나의 주소. host는 "127.0.0.1" 같은 IPv4 주소 문자열 */
struct backend {
    char host[HOST_SIZE];
    int port;
};

/* 라우팅 규칙 하나: 경로가 prefix로 시작하면 backends 중 하나로 보낸다 */
struct route {
    char prefix[PREFIX_SIZE];
    struct backend backends[MAX_BACKENDS];
    int backend_count;
    int next; /* 다음 요청을 보낼 백엔드의 순번 (라운드로빈) */
};

struct proxy_config {
    struct route routes[MAX_ROUTES];
    int route_count;
};

/*
 * 설정 파일을 읽어 cfg를 채운다. 한 줄에 규칙 하나: "접두사 주소:포트 [주소:포트 ...]"
 * 예) "/api/ 127.0.0.1:8001 127.0.0.1:8003".  빈 줄과 '#'로 시작하는 줄은 건너뛴다.
 * 성공하면 0. 파일을 열 수 없거나 형식이 틀리면 원인을 출력하고 -1.
 */
int load_config(const char *path, struct proxy_config *cfg);

/* 경로가 어느 규칙의 접두사로 시작하는지 찾는다. 위에 적힌 규칙이 먼저다. 없으면 NULL */
struct route *match_route(struct proxy_config *cfg, const char *url_path);

/* 규칙의 백엔드를 차례대로 하나씩 돌려준다 (라운드로빈): 0, 1, ..., 마지막, 다시 0 */
const struct backend *pick_backend(struct route *route);

/* 백엔드에 TCP 연결을 걸어 그 fd를 돌려준다. 실패하면 원인을 출력하고 -1 */
int connect_backend(const struct backend *backend);

/* 클라이언트 연결 하나를 처리한다: 요청을 읽고, 백엔드를 골라 전달하고, 응답을 돌려준다. client_fd는 닫지 않는다 */
void proxy_connection(int client_fd, struct proxy_config *cfg);

#endif /* PROXY_H */
