#ifndef PROXY_H
#define PROXY_H

/*
 * 프록시: 클라이언트의 요청을 받아 다른 서버에 전달하고, 그 서버의 응답을 클라이언트에게 돌려준다.
 * 어느 서버로 보낼지는 요청 줄의 요청 대상 모양으로 갈린다.
 *   Reverse Proxy: 요청 대상이 "/api/users" 같은 경로다. 설정 파일의 규칙으로 프록시가 백엔드를 고른다.
 *   Forward Proxy: 요청 대상이 "http://example.com/a" 같은 절대 URL이다. 클라이언트가 적어 보낸 그 서버로 보낸다.
 */

#define MAX_ROUTES 8
#define MAX_BACKENDS 8
#define PREFIX_SIZE 64
#define HOST_SIZE 64
#define MAX_ALLOWED 8
#define HEALTH_INTERVAL_SEC 2 /* 헬스 체크 스레드가 모든 백엔드를 한 번 검사한 뒤 쉬는 시간 */

/* 백엔드 서버 하나의 주소. host는 "127.0.0.1" 같은 IPv4 주소 문자열 */
struct backend {
    char host[HOST_SIZE];
    int port;
    int alive; /* 헬스 체크 결과: 1이면 살아 있음, 0이면 죽음. 헬스 체크 스레드가 쓰고 pick_backend가 읽는다 (같은 잠금) */
};

/* 라우팅 규칙 하나: 경로가 prefix로 시작하면 backends 중 하나로 보낸다 */
struct route {
    char prefix[PREFIX_SIZE];
    struct backend backends[MAX_BACKENDS];
    int backend_count;
    int next; /* 다음 요청을 보낼 백엔드의 순번 (라운드로빈). 여러 스레드가 함께 쓰므로 pick_backend가 잠금을 잡고 읽고 쓴다 */
};

struct proxy_config {
    struct route routes[MAX_ROUTES];
    int route_count;
    /* Forward Proxy 허용 목록. allowed_count가 0이면 어느 서버로든 보낸다. port가 0이면 그 주소의 모든 포트 */
    struct backend allowed[MAX_ALLOWED];
    int allowed_count;
};

/*
 * 설정 파일을 읽어 cfg를 채운다. 한 줄에 규칙 하나: "접두사 주소:포트 [주소:포트 ...]"
 * 예) "/api/ 127.0.0.1:8001 127.0.0.1:8003".  빈 줄과 '#'로 시작하는 줄은 건너뛴다.
 * "allow 주소[:포트]" 줄은 Forward Proxy가 보내도 되는 서버를 적는다. 한 줄도 없으면 모든 서버를 허용한다.
 * 성공하면 0. 파일을 열 수 없거나 형식이 틀리면 원인을 출력하고 -1.
 */
int load_config(const char *path, struct proxy_config *cfg);

/* 경로가 어느 규칙의 접두사로 시작하는지 찾는다. 위에 적힌 규칙이 먼저다. 없으면 NULL */
struct route *match_route(struct proxy_config *cfg, const char *url_path);

/*
 * 규칙의 백엔드를 차례대로 하나씩 돌려준다 (라운드로빈): 0, 1, ..., 마지막, 다시 0.
 * 죽은 백엔드(alive가 0)는 건너뛴다 (자동 제외). 모두 죽었으면 NULL. 여러 스레드가 동시에 불러도 된다
 */
const struct backend *pick_backend(struct route *route);

/*
 * 헬스 체크 한 바퀴: 설정의 모든 백엔드에 connect만 해 보고 바로 close한다.
 * 연결되면 alive를 1로, 안 되면 0으로 바꾼다. 값이 바뀐 백엔드만 "[Health] ..." 한 줄을 출력한다.
 */
void check_backends(struct proxy_config *cfg);

/*
 * 절대 URL "http://주소[:포트][/경로]"를 나눈다. 주소와 포트는 dest에 넣고(포트가 없으면 80),
 * *path는 target 안에서 경로가 시작하는 곳을 가리킨다 (경로가 없으면 "/").
 * 성공하면 0. "http://"로 시작하지 않거나 주소가 비었거나 포트가 틀리면 -1.
 */
int parse_absolute_url(const char *target, struct backend *dest, const char **path);

/* Forward Proxy가 dest로 보내도 되는지. 허용 목록이 비어 있으면 항상 1 */
int is_allowed(const struct proxy_config *cfg, const struct backend *dest);

/* 서버에 TCP 연결을 걸어 그 fd를 돌려준다. host는 숫자 주소와 이름 모두 된다. 실패하면 원인을 출력하고 -1 */
int connect_backend(const struct backend *backend);

/*
 * 클라이언트 연결 하나를 처리한다: 요청을 읽고, 보낼 서버를 정해 전달하고, 응답을 돌려주기를 반복한다 (keep-alive).
 * 클라이언트와의 연결만 유지한다. 서버와의 연결은 요청마다 새로 맺고 응답 뒤에 닫는다.
 * 반복을 멈추는 때: 요청이 닫자고 했을 때, 클라이언트가 닫았을 때, 5초 동안 다음 요청이 없을 때,
 * 프록시가 에러 응답을 보냈을 때, 서버의 응답에 Content-Length가 없을 때. client_fd는 닫지 않는다.
 */
void proxy_connection(int client_fd, struct proxy_config *cfg);

#endif /* PROXY_H */
