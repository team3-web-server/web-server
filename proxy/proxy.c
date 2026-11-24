/* getaddrinfo를 쓰려면 헤더보다 먼저 있어야 한다 (-std=c11) */
#define _POSIX_C_SOURCE 200809L

#include "proxy.h"

#include "http.h"
#include "net.h"
#include "response.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define LINE_SIZE 512      /* 설정 파일 한 줄의 최대 길이 */
#define REQ_BUF_SIZE 8192  /* 요청 줄과 헤더를 받는 버퍼 크기 */
#define PATH_BUF_SIZE 4096 /* 경로 버퍼 크기 */
#define RELAY_BUF_SIZE 8192 /* 한 번에 읽어 전달하는 크기 */
#define HEADER_VALUE_SIZE 64
#define FIRST_LINE_SIZE 4200 /* 백엔드에 보낼 요청 줄을 만드는 버퍼 크기 */
#define ABSOLUTE_PREFIX "http://"

int load_config(const char *path, struct proxy_config *cfg)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path);
        return -1;
    }

    cfg->route_count = 0;
    cfg->allowed_count = 0;
    char line[LINE_SIZE];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;

        /* strtok은 줄을 공백으로 나눈 칸을 하나씩 돌려준다. 첫 칸이 없으면 빈 줄 */
        char *token = strtok(line, " \t\r\n");
        if (token == NULL || token[0] == '#') {
            continue;
        }
        /* "allow 주소[:포트]": Forward Proxy 허용 목록에 한 칸을 더한다. 포트가 없으면 0(모든 포트) */
        if (strcmp(token, "allow") == 0) {
            token = strtok(NULL, " \t\r\n");
            struct backend *allowed = &cfg->allowed[cfg->allowed_count];
            if (token == NULL || cfg->allowed_count == MAX_ALLOWED) {
                fprintf(stderr, "%s %d번째 줄: allow 뒤에 주소가 있어야 하고, %d개까지입니다\n", path, line_no, MAX_ALLOWED);
                fclose(fp);
                return -1;
            }
            allowed->port = 0;
            if (sscanf(token, "%63[^:]:%d", allowed->host, &allowed->port) < 1) {
                fprintf(stderr, "%s %d번째 줄: allow 주소[:포트] 형식이 아닙니다: %s\n", path, line_no, token);
                fclose(fp);
                return -1;
            }
            cfg->allowed_count++;
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

int is_allowed(const struct proxy_config *cfg, const struct backend *dest)
{
    if (cfg->allowed_count == 0) {
        return 1;
    }
    for (int i = 0; i < cfg->allowed_count; i++) {
        const struct backend *allowed = &cfg->allowed[i];
        if (strcmp(allowed->host, dest->host) == 0 && (allowed->port == 0 || allowed->port == dest->port)) {
            return 1;
        }
    }
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

int parse_absolute_url(const char *target, struct backend *dest, const char **path)
{
    size_t prefix_len = sizeof(ABSOLUTE_PREFIX) - 1;
    if (strncmp(target, ABSOLUTE_PREFIX, prefix_len) != 0) {
        return -1;
    }
    const char *host = target + prefix_len;

    /* 주소는 ':' 또는 '/' 앞까지다 */
    size_t host_len = strcspn(host, ":/");
    if (host_len == 0 || host_len >= HOST_SIZE) {
        return -1;
    }
    memcpy(dest->host, host, host_len);
    dest->host[host_len] = '\0';

    /* ':'가 있으면 그 뒤가 포트다. 없으면 HTTP의 기본 포트 80 */
    const char *rest = host + host_len;
    dest->port = 80;
    if (*rest == ':') {
        char *end;
        long port = strtol(rest + 1, &end, 10);
        if (end == rest + 1 || port < 1 || port > 65535) {
            return -1;
        }
        dest->port = (int)port;
        rest = end;
    }

    if (*rest == '/') {
        *path = rest;
    } else if (*rest == '\0') {
        *path = "/";
    } else {
        return -1;
    }
    return 0;
}

int connect_backend(const struct backend *backend)
{
    /*
     * getaddrinfo: 주소 문자열("127.0.0.1" 또는 "example.com")과 포트를 connect에 넘길 수 있는 주소 구조체로 바꾼다.
     * 이름이면 DNS에 물어 IP 주소를 알아낸다. 결과는 목록으로 오고, 다 쓰면 freeaddrinfo로 해제한다.
     */
    char port_text[8];
    snprintf(port_text, sizeof(port_text), "%d", backend->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       /* IPv4 */
    hints.ai_socktype = SOCK_STREAM; /* TCP */
    struct addrinfo *result;
    int err = getaddrinfo(backend->host, port_text, &hints, &result);
    if (err != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", backend->host, gai_strerror(err));
        return -1;
    }

    int fd = create_tcp_socket();
    if (fd == -1) {
        freeaddrinfo(result);
        return -1;
    }
    /* connect: 이 소켓으로 그 주소에 TCP 연결을 건다 (3-way handshake). 프록시가 그 서버의 클라이언트가 된다 */
    if (connect(fd, result->ai_addr, result->ai_addrlen) == -1) {
        perror("connect");
        close(fd);
        freeaddrinfo(result);
        return -1;
    }
    freeaddrinfo(result);
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

    struct http_request req;
    if (parse_request_line(buf, &req) == -1) {
        send_error(client_fd, 400);
        return;
    }

    /*
     * 2) 보낼 서버를 정한다.
     * 요청 대상이 "http://"로 시작하면 Forward Proxy: 클라이언트가 적어 보낸 서버로 보낸다.
     * 아니면 Reverse Proxy: 경로로 라우팅 규칙을 찾고 백엔드를 차례대로 하나 고른다.
     */
    struct backend dest;
    const char *forward_path = NULL; /* Forward Proxy일 때만 쓴다: 절대 URL에서 경로 부분 */
    if (strncmp(req.target, ABSOLUTE_PREFIX, sizeof(ABSOLUTE_PREFIX) - 1) == 0) {
        if (parse_absolute_url(req.target, &dest, &forward_path) == -1) {
            free_request(&req);
            send_error(client_fd, 400);
            return;
        }
        /* 허용 목록이 있으면 목록에 있는 서버로만 보낸다. 없는 서버면 연결하지 않고 403 */
        if (!is_allowed(cfg, &dest)) {
            printf("[Forward] %s %s → 허용 목록에 없음 (403)\n", req.method, req.target);
            free_request(&req);
            send_error(client_fd, 403);
            return;
        }
        printf("[Forward] %s %s → %s:%d\n", req.method, req.target, dest.host, dest.port);
    } else {
        char url_path[PATH_BUF_SIZE];
        const char *query;
        if (split_target(req.target, url_path, sizeof(url_path), &query) == -1) {
            free_request(&req);
            send_error(client_fd, 414);
            return;
        }
        struct route *route = match_route(cfg, url_path);
        if (route == NULL) {
            printf("[Reverse] %s %s → 맞는 규칙 없음 (404)\n", req.method, req.target);
            free_request(&req);
            send_error(client_fd, 404);
            return;
        }
        dest = *pick_backend(route);
        printf("[Reverse] %s %s → %s:%d\n", req.method, req.target, dest.host, dest.port);
    }

    /* 3) 그 서버에 연결한다. 연결하지 못하면 502 Bad Gateway */
    int backend_fd = connect_backend(&dest);
    if (backend_fd == -1) {
        free_request(&req);
        send_error(client_fd, 502);
        return;
    }

    /*
     * 4) 요청을 서버에 쓴다.
     * Reverse Proxy: 받은 바이트(요청 줄 + 헤더 + 함께 들어온 본문 앞부분)를 그대로 쓴다.
     * Forward Proxy: 요청 줄만 새로 만든다. 서버는 절대 URL이 아니라 경로를 받아야 하므로 "메서드 경로 버전"으로 바꾼다.
     *   응답 뒤에 서버가 연결을 닫아야 응답의 끝을 알 수 있으므로, 버전을 HTTP/1.0으로 하고 "Connection: close"를 넣는다.
     *   둘째 줄부터(헤더, 빈 줄, 본문 앞부분)는 받은 그대로 이어서 쓴다.
     */
    int written;
    if (forward_path != NULL) {
        char first[FIRST_LINE_SIZE];
        int first_len = snprintf(first, sizeof(first), "%s %s HTTP/1.0\r\nConnection: close\r\n", req.method, forward_path);
        const char *second_line = strstr(buf, "\r\n") + 2;
        written = first_len < (int)sizeof(first) && write_all(backend_fd, first, (size_t)first_len) == 0 &&
                  write_all(backend_fd, second_line, len - (size_t)(second_line - buf)) == 0;
    } else {
        written = write_all(backend_fd, buf, len) == 0;
    }
    free_request(&req);
    if (!written) {
        perror("write (서버)");
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

    /* 6) 서버가 연결을 닫을 때까지 응답을 읽어 클라이언트에게 그대로 쓴다. 아무것도 오지 않았으면 502 */
    size_t sent = relay_until_close(backend_fd, client_fd);
    close(backend_fd);
    if (sent == 0) {
        send_error(client_fd, 502);
        return;
    }
    printf("응답 %zu바이트를 클라이언트에게 전달\n", sent);
}
