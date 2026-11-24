/* getaddrinfo를 쓰려면 헤더보다 먼저 있어야 한다 (-std=c11) */
#define _POSIX_C_SOURCE 200809L

#include "proxy.h"

#include "http.h"
#include "net.h"
#include "response.h"

#include <pthread.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define LINE_SIZE 512      /* 설정 파일 한 줄의 최대 길이 */
#define REQ_BUF_SIZE 8192  /* 요청 줄과 헤더를 받는 버퍼 크기 */
#define PATH_BUF_SIZE 4096 /* 경로 버퍼 크기 */
#define RELAY_BUF_SIZE 8192 /* 한 번에 읽어 전달하는 크기 */
#define HEADER_VALUE_SIZE 64
#define FIRST_LINE_SIZE 4200 /* 백엔드에 보낼 요청 줄을 만드는 버퍼 크기 */
#define ABSOLUTE_PREFIX "http://"
#define IDLE_TIMEOUT_SEC 5 /* 이 시간 동안 다음 요청이 오지 않으면 클라이언트와의 연결을 닫는다 */

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
            backend->alive = 1; /* 처음에는 살아 있다고 본다. 프록시가 시작하자마자 헬스 체크 스레드가 실제 상태로 고친다 */
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

/*
 * 순번(next)과 헬스 체크 결과(alive)를 읽고 쓰는 동안 잡는 잠금.
 * 요청을 처리하는 스레드들과 헬스 체크 스레드가 이 값들을 함께 쓴다.
 */
static pthread_mutex_t next_lock = PTHREAD_MUTEX_INITIALIZER;

const struct backend *pick_backend(struct route *route)
{
    const struct backend *picked = NULL;
    pthread_mutex_lock(&next_lock);
    /* next부터 한 바퀴(backend_count번)만 본다. 죽은 백엔드는 건너뛰고, 살아 있는 첫 백엔드를 고른다 */
    for (int i = 0; i < route->backend_count; i++) {
        const struct backend *backend = &route->backends[route->next];
        route->next = (route->next + 1) % route->backend_count;
        if (backend->alive) {
            picked = backend;
            break;
        }
    }
    pthread_mutex_unlock(&next_lock);
    return picked;
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

/* 서버에 TCP 연결을 건다. quiet가 1이면 connect 실패의 원인을 출력하지 않는다 (헬스 체크는 2초마다 실패할 수 있다) */
static int connect_to(const struct backend *backend, int quiet)
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
        if (!quiet) {
            perror("connect");
        }
        close(fd);
        freeaddrinfo(result);
        return -1;
    }
    freeaddrinfo(result);
    return fd;
}

int connect_backend(const struct backend *backend)
{
    return connect_to(backend, 0);
}

void check_backends(struct proxy_config *cfg)
{
    for (int i = 0; i < cfg->route_count; i++) {
        struct route *route = &cfg->routes[i];
        for (int j = 0; j < route->backend_count; j++) {
            struct backend *backend = &route->backends[j];

            /* connect는 오래 걸릴 수 있으므로 잠금을 잡지 않고 한다. 연결되면 살아 있는 것이고, 확인만 했으니 바로 닫는다 */
            int fd = connect_to(backend, 1);
            int alive = fd != -1;
            if (fd != -1) {
                close(fd);
            }

            /* alive를 쓰는 동안만 잠금을 잡는다. pick_backend가 같은 잠금을 잡고 이 값을 읽는다 */
            pthread_mutex_lock(&next_lock);
            int changed = backend->alive != alive;
            backend->alive = alive;
            pthread_mutex_unlock(&next_lock);

            if (changed) {
                printf("[Health] %s:%d %s\n", backend->host, backend->port,
                       alive ? "연결됨 → 다시 요청을 보냄 (자동 복귀)" : "연결 실패 → 요청을 보내지 않음 (자동 제외)");
            }
        }
    }
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

/*
 * 백엔드 응답의 헤더(head, 길이 head_len: 상태 줄부터 빈 줄까지)를 클라이언트에게 쓴다.
 * 백엔드는 "Connection: close"로 응답하지만 클라이언트와의 연결은 유지할 것이므로,
 * Connection 줄을 빼고 "Connection: keep-alive"를 넣는다. 나머지 줄은 그대로 옮긴다. 성공하면 0.
 */
static int write_head_keep_alive(int client_fd, const char *head, size_t head_len)
{
    static const char keep_line[] = "Connection: keep-alive\r\n";
    const char *end = head + head_len - 2; /* 마지막 빈 줄("\r\n")의 시작 */
    const char *line = head;
    while (line < end) {
        const char *next = strstr(line, "\r\n") + 2;
        if (strncasecmp(line, "Connection:", 11) != 0 && write_all(client_fd, line, (size_t)(next - line)) == -1) {
            return -1;
        }
        line = next;
    }
    if (write_all(client_fd, keep_line, sizeof(keep_line) - 1) == -1 || write_all(client_fd, "\r\n", 2) == -1) {
        return -1;
    }
    return 0;
}

/*
 * 백엔드의 응답을 클라이언트에게 옮긴다. 백엔드가 연결을 닫을 때까지가 응답 하나다.
 * keep_alive가 1이고 응답에 Content-Length가 있으면 Connection 헤더를 keep-alive로 바꿔 보내고 1을 돌려준다.
 *   (Content-Length가 없는 응답은 클라이언트가 "연결이 닫힘"으로만 끝을 알 수 있으므로 유지할 수 없다.)
 * 그 밖에는 받은 그대로 옮기고 0을 돌려준다. *sent에는 옮긴 바이트 수가 들어간다.
 */
static int relay_response(int backend_fd, int client_fd, int keep_alive, size_t *sent)
{
    char head[REQ_BUF_SIZE];
    size_t len = 0;
    int result = read_request_head(backend_fd, head, sizeof(head), &len);
    *sent = len;

    int kept = 0;
    char value[HEADER_VALUE_SIZE];
    const char *blank = result == HEAD_OK ? strstr(head, "\r\n\r\n") : NULL;
    if (keep_alive && blank != NULL && get_header(head, "Content-Length", value, sizeof(value)) == 0) {
        size_t head_len = (size_t)(blank + 4 - head);
        /* 헤더는 Connection 줄을 바꿔서, 헤더와 함께 들어온 본문 앞부분은 그대로 쓴다 */
        if (write_head_keep_alive(client_fd, head, head_len) == -1 ||
            write_all(client_fd, head + head_len, len - head_len) == -1) {
            return 0;
        }
        kept = 1;
    } else if (len > 0 && write_all(client_fd, head, len) == -1) {
        return 0;
    }
    if (result == HEAD_OK || result == HEAD_TOO_LARGE) {
        *sent += relay_until_close(backend_fd, client_fd);
    }
    return kept;
}

/*
 * 요청 하나를 처리한다: 보낼 서버를 정해 요청을 전달하고, 응답을 클라이언트에게 돌려준다.
 * buf(길이 len)에는 요청 줄과 헤더가 들어 있고, 본문 앞부분이나 다음 요청의 앞부분이 이어져 있을 수 있다.
 * *used에는 buf에서 이 요청이 쓴 바이트 수를 넣는다.
 * 클라이언트와의 연결을 유지해도 되면 1, 닫아야 하면 0을 돌려준다.
 */
static int proxy_request(int client_fd, struct proxy_config *cfg, const char *buf, size_t len, size_t *used)
{
    *used = len;

    struct http_request req;
    if (parse_request_line(buf, &req) == -1) {
        send_error(client_fd, 400);
        return 0;
    }
    int keep_alive = wants_keep_alive(buf, strcmp(req.version, "HTTP/1.1") == 0);

    /*
     * 1) 보낼 서버를 정한다.
     * 요청 대상이 "http://"로 시작하면 Forward Proxy: 클라이언트가 적어 보낸 서버로 보낸다.
     * 아니면 Reverse Proxy: 경로로 라우팅 규칙을 찾고 백엔드를 차례대로 하나 고른다.
     */
    struct backend dest;
    const char *forward_path = NULL; /* Forward Proxy일 때만 쓴다: 절대 URL에서 경로 부분 */
    if (strncmp(req.target, ABSOLUTE_PREFIX, sizeof(ABSOLUTE_PREFIX) - 1) == 0) {
        if (parse_absolute_url(req.target, &dest, &forward_path) == -1) {
            free_request(&req);
            send_error(client_fd, 400);
            return 0;
        }
        /* 허용 목록이 있으면 목록에 있는 서버로만 보낸다. 없는 서버면 연결하지 않고 403 */
        if (!is_allowed(cfg, &dest)) {
            printf("[Forward] %s %s → 허용 목록에 없음 (403)\n", req.method, req.target);
            free_request(&req);
            send_error(client_fd, 403);
            return 0;
        }
        printf("[Forward] %s %s → %s:%d\n", req.method, req.target, dest.host, dest.port);
    } else {
        char url_path[PATH_BUF_SIZE];
        const char *query;
        if (split_target(req.target, url_path, sizeof(url_path), &query) == -1) {
            free_request(&req);
            send_error(client_fd, 414);
            return 0;
        }
        struct route *route = match_route(cfg, url_path);
        if (route == NULL) {
            printf("[Reverse] %s %s → 맞는 규칙 없음 (404)\n", req.method, req.target);
            free_request(&req);
            send_error(client_fd, 404);
            return 0;
        }
        const struct backend *picked = pick_backend(route);
        if (picked == NULL) {
            /* 이 규칙의 백엔드가 모두 죽어 있다. 연결을 시도하지 않고 503 Service Unavailable */
            printf("[Reverse] %s %s → 살아 있는 백엔드 없음 (503)\n", req.method, req.target);
            free_request(&req);
            send_error(client_fd, 503);
            return 0;
        }
        dest = *picked;
        printf("[Reverse] %s %s → %s:%d\n", req.method, req.target, dest.host, dest.port);
    }

    /* 2) 그 서버에 연결한다. 연결하지 못하면 502 Bad Gateway. 서버와의 연결은 요청마다 새로 맺는다 */
    int backend_fd = connect_backend(&dest);
    if (backend_fd == -1) {
        free_request(&req);
        send_error(client_fd, 502);
        return 0;
    }

    /*
     * 이 요청의 본문 길이와, buf에서 이 요청이 차지하는 범위를 구한다.
     * buf에 본문보다 더 들어와 있으면 그 뒤는 다음 요청의 앞부분이므로 서버에 보내지 않는다.
     */
    char value[HEADER_VALUE_SIZE];
    size_t content_length = 0;
    if (get_header(buf, "Content-Length", value, sizeof(value)) != 0 || parse_content_length(value, &content_length) != 0) {
        content_length = 0;
    }
    size_t head_len = (size_t)(strstr(buf, "\r\n\r\n") + 4 - buf);
    size_t body_received = len - head_len;
    if (body_received > content_length) {
        body_received = content_length;
    }
    *used = head_len + body_received;

    /*
     * 3) 요청을 서버에 쓴다. 요청 줄만 새로 만들고, 둘째 줄부터(헤더, 빈 줄, 본문 앞부분)는 받은 그대로 이어서 쓴다.
     * 버전을 HTTP/1.0으로 하고 "Connection: close"를 넣는다. 프록시는 서버가 연결을 닫는 것으로 응답의 끝을 알기 때문이다.
     * Forward Proxy에서는 요청 대상도 바꾼다. 서버는 절대 URL이 아니라 경로를 받아야 한다.
     */
    const char *backend_target = forward_path != NULL ? forward_path : req.target;
    char first[FIRST_LINE_SIZE];
    int first_len = snprintf(first, sizeof(first), "%s %s HTTP/1.0\r\nConnection: close\r\n", req.method, backend_target);
    const char *second_line = strstr(buf, "\r\n") + 2;
    int written = first_len < (int)sizeof(first) && write_all(backend_fd, first, (size_t)first_len) == 0 &&
                  write_all(backend_fd, second_line, *used - (size_t)(second_line - buf)) == 0;
    free_request(&req);
    if (!written) {
        perror("write (서버)");
        close(backend_fd);
        send_error(client_fd, 502);
        return 0;
    }

    /* 4) 본문이 더 남았으면(Content-Length - 이미 받은 본문) 클라이언트에서 읽어 서버로 옮긴다 */
    if (content_length > body_received && relay_bytes(client_fd, backend_fd, content_length - body_received) == -1) {
        close(backend_fd);
        return 0;
    }

    /* 5) 서버가 연결을 닫을 때까지 응답을 읽어 클라이언트에게 쓴다. 아무것도 오지 않았으면 502 */
    size_t sent;
    int kept = relay_response(backend_fd, client_fd, keep_alive, &sent);
    close(backend_fd);
    if (sent == 0) {
        send_error(client_fd, 502);
        return 0;
    }
    printf("응답 %zu바이트를 클라이언트에게 전달\n", sent);
    return kept;
}

void proxy_connection(int client_fd, struct proxy_config *cfg)
{
    /* 다음 요청을 기다리는 read가 IDLE_TIMEOUT_SEC초 넘게 멈춰 있으면 -1(EAGAIN)로 돌아오게 한다 */
    struct timeval timeout = {IDLE_TIMEOUT_SEC, 0};
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
        perror("setsockopt");
    }
    /* 응답을 write 여러 번으로 보내므로, 커널이 작은 데이터를 모으느라 늦추지 않게 한다 (웹 서버와 같은 이유) */
    int on = 1;
    if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == -1) {
        perror("setsockopt");
    }

    /* buf와 len은 반복문 밖에 둔다. 앞 요청과 함께 들어온 다음 요청의 앞부분을 다음 바퀴에서 이어 쓴다 */
    char buf[REQ_BUF_SIZE];
    size_t len = 0;

    /* keep-alive: 클라이언트와의 연결 하나에서 [요청 읽기 → 서버에 전달 → 응답 돌려주기]를 반복한다 */
    for (;;) {
        int head = read_request_head(client_fd, buf, sizeof(buf), &len);
        if (head == HEAD_TIMEOUT) {
            printf("%d초 동안 요청이 없어 클라이언트와의 연결을 닫습니다\n", IDLE_TIMEOUT_SEC);
            return;
        }
        if (head == HEAD_ERROR || head == HEAD_CLOSED) {
            return;
        }
        if (head == HEAD_TOO_LARGE) {
            send_error(client_fd, 431);
            return;
        }

        size_t used;
        if (!proxy_request(client_fd, cfg, buf, len, &used)) {
            return;
        }
        printf("클라이언트와의 연결을 유지하고 다음 요청을 기다립니다\n");

        /* 이 요청이 쓴 만큼을 버리고, 남은 바이트(다음 요청의 앞부분)를 buf의 맨 앞으로 옮긴다 */
        len -= used;
        memmove(buf, buf + used, len);
    }
}
