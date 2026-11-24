#include "proxy.h"

#include "http.h"
#include "net.h"
#include "response.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define LINE_SIZE 512      /* 설정 파일 한 줄의 최대 길이 */
#define REQ_BUF_SIZE 8192  /* 요청 줄과 헤더를 받는 버퍼 크기 */
#define PATH_BUF_SIZE 4096 /* 경로 버퍼 크기 */
#define RELAY_BUF_SIZE 8192 /* 한 번에 읽어 전달하는 크기 */
#define HEADER_VALUE_SIZE 64

int load_config(const char *path, struct proxy_config *cfg)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path);
        return -1;
    }

    cfg->route_count = 0;
    char line[LINE_SIZE];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;

        /* strtok은 줄을 공백으로 나눈 칸을 하나씩 돌려준다. 첫 칸이 없으면 빈 줄 */
        char *token = strtok(line, " \t\r\n");
        if (token == NULL || token[0] == '#') {
            continue;
        }
        if (token[0] != '/' || strlen(token) >= PREFIX_SIZE || cfg->route_count == MAX_ROUTES) {
            fprintf(stderr, "%s %d번째 줄: 접두사는 '/'로 시작해야 하고, 규칙은 %d개까지입니다\n", path, line_no,
                    MAX_ROUTES);
            fclose(fp);
            return -1;
        }

        struct route *route = &cfg->routes[cfg->route_count];
        snprintf(route->prefix, sizeof(route->prefix), "%s", token);
        route->backend_count = 0;
        route->next = 0;

        /* 나머지 칸은 "주소:포트". %63[^:]는 ':' 전까지 최대 63글자를 읽는다 */
        while ((token = strtok(NULL, " \t\r\n")) != NULL) {
            struct backend *backend = &route->backends[route->backend_count];
            if (route->backend_count == MAX_BACKENDS ||
                sscanf(token, "%63[^:]:%d", backend->host, &backend->port) != 2 || backend->port < 1 ||
                backend->port > 65535) {
                fprintf(stderr, "%s %d번째 줄: 백엔드는 주소:포트 형식이고 %d개까지입니다: %s\n", path, line_no,
                        MAX_BACKENDS, token);
                fclose(fp);
                return -1;
            }
            route->backend_count++;
        }
        if (route->backend_count == 0) {
            fprintf(stderr, "%s %d번째 줄: 백엔드가 하나도 없습니다\n", path, line_no);
            fclose(fp);
            return -1;
        }
        cfg->route_count++;
    }
    fclose(fp);
    return 0;
}

struct route *match_route(struct proxy_config *cfg, const char *url_path)
{
    for (int i = 0; i < cfg->route_count; i++) {
        struct route *route = &cfg->routes[i];
        if (strncmp(url_path, route->prefix, strlen(route->prefix)) == 0) {
            return route;
        }
    }
    return NULL;
}

const struct backend *pick_backend(struct route *route)
{
    const struct backend *backend = &route->backends[route->next];
    route->next = (route->next + 1) % route->backend_count;
    return backend;
}

int connect_backend(const struct backend *backend)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)backend->port);
    /* inet_pton: "127.0.0.1" 같은 문자열을 네트워크 바이트 순서의 주소 숫자로 바꾼다. 형식이 틀리면 0 */
    if (inet_pton(AF_INET, backend->host, &addr.sin_addr) != 1) {
        fprintf(stderr, "백엔드 주소가 IPv4 형식이 아닙니다: %s\n", backend->host);
        return -1;
    }

    int fd = create_tcp_socket();
    if (fd == -1) {
        return -1;
    }
    /* connect: 이 소켓으로 그 주소에 TCP 연결을 건다 (3-way handshake). 프록시가 백엔드의 클라이언트가 된다 */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* from에서 len바이트를 읽어 to에 그대로 쓴다. 다 옮기면 0, 도중에 끊기면 -1 */
static int relay_bytes(int from, int to, size_t len)
{
    char buf[RELAY_BUF_SIZE];
    while (len > 0) {
        size_t want = len < sizeof(buf) ? len : sizeof(buf);
        ssize_t n = read(from, buf, want);
        if (n <= 0 || write_all(to, buf, (size_t)n) == -1) {
            return -1;
        }
        len -= (size_t)n;
    }
    return 0;
}

/* from이 닫힐 때(read가 0)까지 읽어 to에 그대로 쓴다. 옮긴 바이트 수를 돌려준다 */
static size_t relay_until_close(int from, int to)
{
    char buf[RELAY_BUF_SIZE];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(from, buf, sizeof(buf));
        if (n <= 0 || write_all(to, buf, (size_t)n) == -1) {
            break;
        }
        total += (size_t)n;
    }
    return total;
}

void proxy_connection(int client_fd, struct proxy_config *cfg)
{
    /* 1) 클라이언트의 요청 줄과 헤더를 읽는다. 본문 앞부분이 함께 들어올 수 있다 */
    char buf[REQ_BUF_SIZE];
    size_t len = 0;
    int head = read_request_head(client_fd, buf, sizeof(buf), &len);
    if (head == HEAD_ERROR || head == HEAD_CLOSED) {
        return;
    }
    if (head == HEAD_TOO_LARGE) {
        send_error(client_fd, 431);
        return;
    }

    /* 2) 요청 줄에서 경로를 꺼내 라우팅 규칙을 찾는다 */
    struct http_request req;
    if (parse_request_line(buf, &req) == -1) {
        send_error(client_fd, 400);
        return;
    }
    char url_path[PATH_BUF_SIZE];
    const char *query;
    if (split_target(req.target, url_path, sizeof(url_path), &query) == -1) {
        free_request(&req);
        send_error(client_fd, 414);
        return;
    }
    struct route *route = match_route(cfg, url_path);
    if (route == NULL) {
        printf("%s %s → 맞는 규칙 없음 (404)\n", req.method, req.target);
        free_request(&req);
        send_error(client_fd, 404);
        return;
    }

    /* 3) 백엔드를 차례대로 하나 골라 연결한다. 연결하지 못하면 502 Bad Gateway */
    const struct backend *backend = pick_backend(route);
    printf("%s %s → %s:%d\n", req.method, req.target, backend->host, backend->port);
    free_request(&req);
    int backend_fd = connect_backend(backend);
    if (backend_fd == -1) {
        send_error(client_fd, 502);
        return;
    }

    /* 4) 받은 요청(요청 줄 + 헤더 + 함께 들어온 본문 앞부분)을 그대로 백엔드에 쓴다 */
    if (write_all(backend_fd, buf, len) == -1) {
        perror("write (백엔드)");
        close(backend_fd);
        send_error(client_fd, 502);
        return;
    }

    /* 5) 본문이 더 남았으면(Content-Length - 이미 받은 본문) 클라이언트에서 읽어 백엔드로 옮긴다 */
    char value[HEADER_VALUE_SIZE];
    size_t content_length;
    if (get_header(buf, "Content-Length", value, sizeof(value)) == 0 &&
        parse_content_length(value, &content_length) == 0) {
        size_t body_received = len - (size_t)(strstr(buf, "\r\n\r\n") + 4 - buf);
        if (content_length > body_received) {
            relay_bytes(client_fd, backend_fd, content_length - body_received);
        }
    }

    /* 6) 백엔드가 연결을 닫을 때까지 응답을 읽어 클라이언트에게 그대로 쓴다. 아무것도 오지 않았으면 502 */
    size_t sent = relay_until_close(backend_fd, client_fd);
    close(backend_fd);
    if (sent == 0) {
        send_error(client_fd, 502);
        return;
    }
    printf("응답 %zu바이트를 클라이언트에게 전달\n", sent);
}
