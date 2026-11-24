/* getaddrinfo를 쓰려면 헤더보다 먼저 있어야 한다 (-std=c11) */
#define _POSIX_C_SOURCE 200809L

#include "proxy.h"

#include "cache.h"

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
#include <time.h>
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
    cfg->cache_count = 0;
    char line[LINE_SIZE];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;

        /* strtok은 줄을 공백으로 나눈 칸을 하나씩 돌려준다. 첫 칸이 없으면 빈 줄 */
        char *token = strtok(line, " \t\r\n");
        if (token == NULL || token[0] == '#') {
            continue;
        }
        /* "cache 접두사": 그 접두사로 시작하는 경로의 응답을 캐시에 저장한다 */
        if (strcmp(token, "cache") == 0) {
            token = strtok(NULL, " \t\r\n");
            if (token == NULL || token[0] != '/' || strlen(token) >= PREFIX_SIZE || cfg->cache_count == MAX_CACHE_PREFIXES) {
                fprintf(stderr, "%s %d번째 줄: cache 뒤에는 '/'로 시작하는 접두사가 와야 하고, %d개까지입니다\n", path, line_no,
                        MAX_CACHE_PREFIXES);
                fclose(fp);
                return -1;
            }
            snprintf(cfg->cache_prefixes[cfg->cache_count], PREFIX_SIZE, "%s", token);
            cfg->cache_count++;
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
            backend->idle_count = 0;
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

int is_cache_path(const struct proxy_config *cfg, const char *target)
{
    for (int i = 0; i < cfg->cache_count; i++) {
        if (strncmp(target, cfg->cache_prefixes[i], strlen(cfg->cache_prefixes[i])) == 0) {
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

struct backend *pick_backend(struct route *route)
{
    struct backend *picked = NULL;
    pthread_mutex_lock(&next_lock);
    /* next부터 한 바퀴(backend_count번)만 본다. 죽은 백엔드는 건너뛰고, 살아 있는 첫 백엔드를 고른다 */
    for (int i = 0; i < route->backend_count; i++) {
        struct backend *backend = &route->backends[route->next];
        route->next = (route->next + 1) % route->backend_count;
        if (backend->alive) {
            picked = backend;
            break;
        }
    }
    pthread_mutex_unlock(&next_lock);
    return picked;
}

/*
 * 연결 보관함(idle_fds, idle_since, idle_count)을 읽고 쓰는 동안 잡는 잠금. next_lock, 캐시의 잠금과는 별도다.
 * 꺼내기와 넣기만 감싼다. 꺼낸 연결로 요청을 보내고 응답을 받는 동안에는 잡지 않는다.
 */
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;

static long long pool_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long long (*pool_clock)(void) = pool_now_ms;

int pool_take(struct backend *backend)
{
    int fd = -1;
    int stale[POOL_MAX];
    int stale_count = 0;

    pthread_mutex_lock(&pool_lock);
    long long now = pool_clock();
    /*
     * 가장 나중에 넣은 연결(가장 덜 쉰 연결)부터 본다. 꺼낸 연결은 idle_count를 줄여 보관함에서 뺀다.
     * 빼 오기 때문에 두 스레드가 같은 연결을 동시에 쓰는 일이 없다.
     */
    while (backend->idle_count > 0 && fd == -1) {
        backend->idle_count--;
        int candidate = backend->idle_fds[backend->idle_count];
        if (now - backend->idle_since[backend->idle_count] >= POOL_IDLE_MS) {
            stale[stale_count++] = candidate; /* 너무 오래 쉬었다. 백엔드가 곧 닫거나 이미 닫았을 연결이다 */
        } else {
            fd = candidate;
        }
    }
    pthread_mutex_unlock(&pool_lock);

    /* close와 출력은 잠금을 푼 뒤에 한다 */
    for (int i = 0; i < stale_count; i++) {
        close(stale[i]);
    }
    if (stale_count > 0) {
        printf("[Pool] %s:%d %d초 넘게 쉰 연결 %d개를 닫음\n", backend->host, backend->port, POOL_IDLE_MS / 1000, stale_count);
    }
    return fd;
}

int pool_put(struct backend *backend, int fd)
{
    int count = 0;
    pthread_mutex_lock(&pool_lock);
    if (backend->idle_count < POOL_MAX) {
        backend->idle_fds[backend->idle_count] = fd;
        backend->idle_since[backend->idle_count] = pool_clock();
        backend->idle_count++;
        count = backend->idle_count;
    }
    pthread_mutex_unlock(&pool_lock);

    if (count == 0) {
        close(fd); /* 보관함이 꽉 찼다 */
    }
    return count;
}

int pool_close_all(struct backend *backend)
{
    int fds[POOL_MAX];
    pthread_mutex_lock(&pool_lock);
    int count = backend->idle_count;
    memcpy(fds, backend->idle_fds, sizeof(fds));
    backend->idle_count = 0;
    pthread_mutex_unlock(&pool_lock);

    for (int i = 0; i < count; i++) {
        close(fds[i]);
    }
    return count;
}

void mark_backend_dead(struct backend *backend)
{
    pthread_mutex_lock(&next_lock);
    backend->alive = 0;
    pthread_mutex_unlock(&next_lock);
    /* 죽은 백엔드와의 연결은 다시 쓸 수 없다. next_lock을 푼 뒤에 보관함을 비운다 (잠금 둘을 겹쳐 잡지 않는다) */
    pool_close_all(backend);
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
            /* 죽은 백엔드의 보관함은 비운다. 보관한 연결은 이미 끊겼거나 곧 끊긴다 */
            if (!alive) {
                int closed = pool_close_all(backend);
                if (closed > 0) {
                    printf("[Pool] %s:%d 보관한 연결 %d개를 닫음 (백엔드가 죽음으로 표시됨)\n", backend->host, backend->port, closed);
                }
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

/*
 * 백엔드 응답의 헤더(head, 길이 head_len: 상태 줄부터 빈 줄까지)를 클라이언트에게 쓴다.
 * 응답의 Connection 줄은 프록시와 백엔드 사이의 연결에 대한 것이다. 클라이언트와 프록시 사이의 연결은 따로 정하므로,
 * 그 줄을 빼고 keep이 1이면 "Connection: keep-alive", 0이면 "Connection: close"를 넣는다. 나머지 줄은 그대로 옮긴다. 성공하면 0.
 */
static int write_head(int client_fd, const char *head, size_t head_len, int keep)
{
    const char *conn_line = keep ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    const char *end = head + head_len - 2; /* 마지막 빈 줄("\r\n")의 시작 */
    const char *line = head;
    while (line < end) {
        const char *next = strstr(line, "\r\n") + 2;
        if (strncasecmp(line, "Connection:", 11) != 0 && write_all(client_fd, line, (size_t)(next - line)) == -1) {
            return -1;
        }
        line = next;
    }
    if (write_all(client_fd, conn_line, strlen(conn_line)) == -1 || write_all(client_fd, "\r\n", 2) == -1) {
        return -1;
    }
    return 0;
}

/*
 * 응답의 앞부분(data, 길이 len: 헤더 전체와 그 뒤에 이어진 본문)을 클라이언트에게 쓴다. data는 '\0'으로 끝나야 한다.
 * keep_alive가 1이고 응답에 Content-Length가 있으면 Connection 헤더를 keep-alive로 바꿔 보내고 1을 돌려준다.
 *   (Content-Length가 없는 응답은 클라이언트가 "연결이 닫힘"으로만 끝을 알 수 있으므로 유지할 수 없다.)
 * 그 밖에는 Connection 헤더를 close로 바꿔 보내고 0을 돌려준다. 쓰지 못하면 -1.
 */
static int write_response(int client_fd, const char *data, size_t len, int keep_alive)
{
    char value[HEADER_VALUE_SIZE];
    const char *blank = strstr(data, "\r\n\r\n");
    if (blank != NULL) {
        int keep = keep_alive && get_header(data, "Content-Length", value, sizeof(value)) == 0;
        size_t head_len = (size_t)(blank + 4 - data);
        /* 헤더는 Connection 줄을 바꿔서, 헤더 뒤에 이어진 본문은 그대로 쓴다 */
        if (write_head(client_fd, data, head_len, keep) == -1 || write_all(client_fd, data + head_len, len - head_len) == -1) {
            return -1;
        }
        return keep;
    }
    return write_all(client_fd, data, len) == -1 ? -1 : 0;
}

/* 캐시에 넣을 사본을 모으는 곳. 응답을 클라이언트에게 옮기면서 같은 바이트를 data에 이어 붙인다 */
struct copy_buf {
    char *data;   /* CACHE_OBJECT_MAX + 1바이트 */
    size_t len;
    int too_large; /* 응답이 CACHE_OBJECT_MAX보다 커서 모으기를 그만뒀다 */
    int cut;       /* 응답을 끝까지 옮기지 못했다 (백엔드에서 읽기 오류, 또는 클라이언트가 먼저 끊음). 사본이 잘려 있다 */
};

static void copy_cut(struct copy_buf *copy)
{
    if (copy != NULL) {
        copy->cut = 1;
    }
}

static void copy_append(struct copy_buf *copy, const char *bytes, size_t n)
{
    if (copy == NULL || copy->too_large) {
        return;
    }
    if (copy->len + n > CACHE_OBJECT_MAX) {
        copy->too_large = 1;
        return;
    }
    memcpy(copy->data + copy->len, bytes, n);
    copy->len += n;
}

/*
 * 백엔드의 응답 하나를 클라이언트에게 옮긴다. 응답의 끝은 이렇게 안다.
 *   Content-Length가 있으면: 본문을 그 길이만큼 읽으면 끝이다. 연결이 열려 있어도 끝을 알 수 있다.
 *   Content-Length가 없으면: 백엔드가 연결을 닫을 때까지가 응답이다.
 * 클라이언트와의 연결을 유지해도 되면 1, 아니면 0을 돌려준다. *sent에는 옮긴 바이트 수가 들어간다.
 * copy가 NULL이 아니면 옮긴 바이트를 copy에도 모은다 (캐시에 넣을 사본).
 * *reusable에는 백엔드와의 연결을 다시 써도 되면 1을 넣는다: Content-Length만큼 다 읽었고,
 * 백엔드의 응답이 HTTP/1.1이며 "Connection: close"가 아닐 때다. 그 밖에는 0이고, 부른 쪽이 연결을 닫아야 한다.
 */
static int relay_response(int backend_fd, int client_fd, int keep_alive, size_t *sent, struct copy_buf *copy, int *reusable)
{
    *reusable = 0;
    char head[REQ_BUF_SIZE];
    size_t len = 0;
    int result = read_request_head(backend_fd, head, sizeof(head), &len);
    *sent = len;
    copy_append(copy, head, len);

    int kept = 0;
    if (result == HEAD_OK) {
        kept = write_response(client_fd, head, len, keep_alive);
        if (kept == -1) {
            copy_cut(copy);
            return 0;
        }
    } else if (len > 0 && write_all(client_fd, head, len) == -1) {
        copy_cut(copy);
        return 0;
    }
    if (result != HEAD_OK && result != HEAD_TOO_LARGE) {
        return kept;
    }

    char buf[RELAY_BUF_SIZE];
    char value[HEADER_VALUE_SIZE];
    size_t content_length = 0;
    size_t body_have = 0; /* 헤더와 함께 이미 들어온 본문의 길이 */
    int has_length = 0;
    if (result == HEAD_OK) {
        body_have = len - (size_t)(strstr(head, "\r\n\r\n") + 4 - head);
        has_length = get_header(head, "Content-Length", value, sizeof(value)) == 0 &&
                     parse_content_length(value, &content_length) == 0 && body_have <= content_length;
    }
    if (has_length) {
        /* 본문의 남은 길이만큼만 읽는다. 다 읽으면 백엔드가 연결을 닫지 않았어도 응답은 끝났다 */
        size_t left = content_length - body_have;
        while (left > 0) {
            ssize_t n = read(backend_fd, buf, left < sizeof(buf) ? left : sizeof(buf));
            if (n <= 0 || write_all(client_fd, buf, (size_t)n) == -1) {
                copy_cut(copy);
                return kept;
            }
            *sent += (size_t)n;
            copy_append(copy, buf, (size_t)n);
            left -= (size_t)n;
        }
        /* 백엔드가 "Connection: close"라고 했으면 곧 닫을 연결이므로 다시 쓰지 않는다 */
        int says_close = get_header(head, "Connection", value, sizeof(value)) == 0 && strcasecmp(value, "close") == 0;
        *reusable = strncmp(head, "HTTP/1.1", 8) == 0 && !says_close;
        return kept;
    }
    for (;;) {
        ssize_t n = read(backend_fd, buf, sizeof(buf));
        if (n == 0) {
            break; /* 백엔드가 연결을 닫았다: 응답의 끝 */
        }
        if (n < 0 || write_all(client_fd, buf, (size_t)n) == -1) {
            copy_cut(copy);
            break;
        }
        *sent += (size_t)n;
        copy_append(copy, buf, (size_t)n);
    }
    return kept;
}

/*
 * 모은 사본(data, len)을 캐시에 넣어도 되는 응답인지 본다: 상태 코드가 200이고,
 * Content-Length가 있으면 본문이 그 길이만큼 다 온 응답만 넣는다 (도중에 끊긴 응답을 넣지 않는다).
 * Content-Length가 없는 응답이 도중에 끊긴 경우는 여기서 알 수 없어서, 옮기는 쪽이 copy_buf의 cut에 적어 둔다.
 */
static int is_cacheable(const char *data, size_t len)
{
    /* 상태 줄 "HTTP/1.x 200 "은 13바이트다. 그보다 짧으면 data + 8부터 읽을 수 없다 */
    if (len < 13 || strncmp(data, "HTTP/1.", 7) != 0 || strncmp(data + 8, " 200 ", 5) != 0) {
        return 0;
    }
    const char *blank = strstr(data, "\r\n\r\n");
    if (blank == NULL) {
        return 0;
    }
    char value[HEADER_VALUE_SIZE];
    size_t content_length;
    if (get_header(data, "Content-Length", value, sizeof(value)) == 0 && parse_content_length(value, &content_length) == 0) {
        return len - (size_t)(blank + 4 - data) == content_length;
    }
    return 1;
}

/*
 * 요청을 서버에 쓴다. 요청 줄은 새로 만들고, 헤더는 Connection 줄만 빼고 받은 그대로, 본문 앞부분도 그대로 쓴다.
 * buf에서 이 요청이 차지하는 범위는 used바이트이고, 그중 헤더는 head_len바이트다.
 * reuse가 1이면 (Reverse Proxy) 버전을 HTTP/1.1로 하고 "Connection: keep-alive"를 넣는다. 응답 뒤에 연결을 다시 쓰려는 것이다.
 * reuse가 0이면 (Forward Proxy) HTTP/1.0과 "Connection: close"를 넣는다. 응답 뒤에 서버가 연결을 닫는다.
 * 클라이언트가 보낸 Connection 줄은 클라이언트와 프록시 사이의 연결에 대한 것이라서 서버에 넘기지 않는다.
 * HTTP/1.1 요청에는 Host 헤더가 꼭 있어야 한다 (없으면 서버가 400으로 답한다). HTTP/1.0 클라이언트는 Host를 보내지 않을 수 있으므로,
 * 요청에 Host가 없으면 보낼 서버(dest)의 주소로 Host 줄을 넣는다.
 * 한 번의 write로 보내려고 한 버퍼에 모은다. 성공하면 0, 실패하면 -1.
 */
static int send_request(int backend_fd, const struct backend *dest, const char *method, const char *target, int reuse,
                        const char *buf, size_t head_len, size_t used)
{
    char out[FIRST_LINE_SIZE + REQ_BUF_SIZE];
    int n = snprintf(out, FIRST_LINE_SIZE, "%s %s %s\r\nConnection: %s\r\n", method, target, reuse ? "HTTP/1.1" : "HTTP/1.0",
                     reuse ? "keep-alive" : "close");
    char value[HEADER_VALUE_SIZE];
    if (n < FIRST_LINE_SIZE && reuse && get_header(buf, "Host", value, sizeof(value)) == 1) {
        n += snprintf(out + n, FIRST_LINE_SIZE - (size_t)n, "Host: %s:%d\r\n", dest->host, dest->port);
    }
    if (n >= FIRST_LINE_SIZE) {
        return -1;
    }
    size_t out_len = (size_t)n;

    const char *end = buf + head_len - 2;        /* 마지막 빈 줄("\r\n")의 시작 */
    const char *line = strstr(buf, "\r\n") + 2; /* 둘째 줄: 첫 헤더 */
    while (line < end) {
        const char *next = strstr(line, "\r\n") + 2;
        if (strncasecmp(line, "Connection:", 11) != 0) {
            memcpy(out + out_len, line, (size_t)(next - line));
            out_len += (size_t)(next - line);
        }
        line = next;
    }
    /* 빈 줄과, 헤더와 함께 들어온 본문 앞부분 */
    memcpy(out + out_len, end, used - (head_len - 2));
    out_len += used - (head_len - 2);
    return write_all(backend_fd, out, out_len);
}

/* 서버와 요청 하나를 주고받은 결과 */
struct exchange {
    int request_failed; /* 요청을 서버에 쓰지 못했다 */
    int body_failed;    /* 요청 본문을 클라이언트에서 서버로 옮기다 끊겼다 */
    size_t sent;        /* 클라이언트에게 옮긴 응답 바이트 수. 0이면 서버에서 아무것도 오지 않았다 */
    int kept;           /* 클라이언트와의 연결을 유지해도 되면 1 */
    int reusable;       /* 서버와의 연결을 다시 써도 되면 1 */
};

/*
 * 서버와 요청 하나를 주고받는다:
 * 3) 요청을 서버에 쓴다. 4) 본문이 더 남았으면(body_left바이트) 클라이언트에서 읽어 서버로 옮긴다.
 * 그리고 서버의 응답을 클라이언트에게 옮긴다. 결과는 *ex에 넣는다. backend_fd는 닫지 않는다.
 */
static void exchange_once(int backend_fd, const struct backend *dest, int client_fd, const struct http_request *req,
                          const char *target, int reuse,
                          const char *buf, size_t head_len, size_t used, size_t body_left, int keep_alive,
                          struct copy_buf *copy, struct exchange *ex)
{
    memset(ex, 0, sizeof(*ex));
    if (send_request(backend_fd, dest, req->method, target, reuse, buf, head_len, used) == -1) {
        perror("write (서버)");
        ex->request_failed = 1;
        return;
    }
    if (body_left > 0 && relay_bytes(client_fd, backend_fd, body_left) == -1) {
        ex->body_failed = 1;
        return;
    }
    ex->kept = relay_response(backend_fd, client_fd, keep_alive, &ex->sent, copy, &ex->reusable);
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
     * 0) 캐시를 본다. 설정 파일의 "cache 접두사"에 맞는 경로의 GET 요청만 대상이고,
     * 키는 요청 대상 전체("?" 뒤의 쿼리 포함)다.
     * 사본이 있으면(hit) 서버를 고르지도 연결하지도 않고 사본을 클라이언트에게 쓴다.
     */
    int use_cache = strcmp(req.method, "GET") == 0 && is_cache_path(cfg, req.target);
    char *cached;
    size_t cached_len;
    if (use_cache && cache_get(req.target, &cached, &cached_len)) {
        printf("[Cache] HIT %s → 사본 %zu바이트를 보냄 (백엔드에 연결하지 않음)\n", req.target, cached_len);
        free_request(&req);
        int kept = write_response(client_fd, cached, cached_len, keep_alive);
        free(cached);
        return kept == 1;
    }
    /* miss면 백엔드에 물어보기 시작하는 시각을 적어 둔다. 사본의 만료 시각을 여기서부터 센다 */
    long long asked_at = 0;
    if (use_cache) {
        asked_at = cache_clock();
        printf("[Cache] MISS %s\n", req.target);
    }

    /*
     * 요청을 서버에 쓰고, 남은 본문을 옮기고, 응답을 클라이언트에게 옮기는 일은 아래에서 한 번 넘게 할 수 있다
     * (보관한 연결이 닫혀 있어 새 연결로 다시 시도할 때). 캐시 대상 요청이면 응답의 사본을 copy에 모은다.
     */
    struct copy_buf copy = {NULL, 0, 0, 0};
    if (use_cache) {
        copy.data = malloc(CACHE_OBJECT_MAX + 1);
    }
    struct exchange ex;
    const struct backend *dest = NULL; /* 요청을 보낸 서버 (출력용) */
    int backend_fd = -1;

    /*
     * 1) 보낼 서버를 정한다.
     * 요청 대상이 "http://"로 시작하면 Forward Proxy: 클라이언트가 적어 보낸 서버로 보낸다.
     * 아니면 Reverse Proxy: 경로로 라우팅 규칙을 찾고 백엔드를 차례대로 하나 고른다.
     */
    struct backend forward_dest;
    struct backend *picked = NULL; /* Reverse Proxy일 때만 쓴다: 고른 백엔드. 연결 보관함이 여기에 있다 */
    if (strncmp(req.target, ABSOLUTE_PREFIX, sizeof(ABSOLUTE_PREFIX) - 1) == 0) {
        const char *forward_path;
        if (parse_absolute_url(req.target, &forward_dest, &forward_path) == -1) {
            free(copy.data);
            free_request(&req);
            send_error(client_fd, 400);
            return 0;
        }
        dest = &forward_dest;
        printf("[Forward] %s %s → %s:%d\n", req.method, req.target, dest->host, dest->port);

        /* 클라이언트가 정한 서버에 연결한다. 연결하지 못하면 502 Bad Gateway (대신 보낼 다른 서버가 없다) */
        backend_fd = connect_backend(dest);
        if (backend_fd == -1) {
            free(copy.data);
            free_request(&req);
            send_error(client_fd, 502);
            return 0;
        }
        /* 서버는 절대 URL이 아니라 경로를 받아야 한다. 연결은 다시 쓰지 않는다 (HTTP/1.0, Connection: close) */
        exchange_once(backend_fd, dest, client_fd, &req, forward_path, 0, buf, head_len, *used, content_length - body_received,
                      keep_alive, copy.data != NULL ? &copy : NULL, &ex);
    } else {
        char url_path[PATH_BUF_SIZE];
        const char *query;
        if (split_target(req.target, url_path, sizeof(url_path), &query) == -1) {
            free(copy.data);
            free_request(&req);
            send_error(client_fd, 414);
            return 0;
        }
        struct route *route = match_route(cfg, url_path);
        if (route == NULL) {
            printf("[Reverse] %s %s → 맞는 규칙 없음 (404)\n", req.method, req.target);
            free(copy.data);
            free_request(&req);
            send_error(client_fd, 404);
            return 0;
        }
        /*
         * 2) 백엔드를 고르고, 그 백엔드와의 연결을 구해 요청을 주고받는다.
         * 연결은 보관함에 쉬는 것이 있으면 꺼내 다시 쓰고(connect 없음), 없으면 새로 connect한다.
         * 보관한 연결은 본문이 없는 GET 요청에만 쓴다. 보관한 연결은 그사이 백엔드가 닫았을 수 있는데,
         * 그때 다시 보내도 되는 요청이 GET이기 때문이다. POST는 두 번 보내면 두 번 처리될 수 있어서 늘 새 연결로 보낸다.
         *
         * connect하지 못하면 그 백엔드를 바로 제외하고 다음 백엔드로 다시 시도한다 (failover).
         * 제외한 백엔드는 다시 고르지 않으므로, 많아야 백엔드 수만큼 돈다.
         */
        int can_use_pool = strcmp(req.method, "GET") == 0 && content_length == 0;
        int done = 0;
        for (int attempt = 0; attempt < route->backend_count && !done; attempt++) {
            picked = pick_backend(route);
            if (picked == NULL) {
                break;
            }
            dest = picked;
            printf("[Reverse] %s %s → %s:%d\n", req.method, req.target, dest->host, dest->port);

            backend_fd = can_use_pool ? pool_take(picked) : -1;
            if (backend_fd != -1) {
                printf("[Pool] %s:%d 보관한 연결을 다시 씀 (connect 없음)\n", dest->host, dest->port);
                exchange_once(backend_fd, dest, client_fd, &req, req.target, 1, buf, head_len, *used, 0, keep_alive,
                              copy.data != NULL ? &copy : NULL, &ex);
                if (ex.sent > 0) {
                    done = 1;
                    break;
                }
                /* 응답이 한 바이트도 오지 않았다: 보관하는 동안 백엔드가 닫은 연결이다. 클라이언트에게는 아직 아무것도 쓰지 않았다 */
                close(backend_fd);
                printf("[Pool] %s:%d 보관한 연결이 이미 닫혀 있음 → 새 연결로 다시 보냄\n", dest->host, dest->port);
            }

            backend_fd = connect_backend(picked);
            if (backend_fd == -1) {
                mark_backend_dead(picked);
                printf("[Failover] %s:%d 연결 실패 → 제외하고 다음 백엔드로 다시 시도\n", dest->host, dest->port);
                continue;
            }
            printf("[Pool] %s:%d 새로 connect\n", dest->host, dest->port);
            copy.len = 0;
            copy.too_large = 0;
            copy.cut = 0;
            exchange_once(backend_fd, dest, client_fd, &req, req.target, 1, buf, head_len, *used, content_length - body_received,
                          keep_alive, copy.data != NULL ? &copy : NULL, &ex);
            done = 1;
        }
        if (!done) {
            /* 살아 있는 백엔드가 하나도 없다 (처음부터 없었거나, 시도한 백엔드가 모두 실패했다). 503 Service Unavailable */
            printf("[Reverse] %s %s → 살아 있는 백엔드 없음 (503)\n", req.method, req.target);
            free(copy.data);
            free_request(&req);
            send_error(client_fd, 503);
            return 0;
        }
    }

    /*
     * 5) 서버와의 연결을 정리한다. Reverse Proxy이고 다시 써도 되는 연결이면 닫지 않고 그 백엔드의 보관함에 넣는다.
     * 그 밖에는(Forward Proxy, 응답에 Content-Length가 없음, 백엔드가 Connection: close라고 함, 도중에 끊김) 닫는다.
     */
    if (picked != NULL && ex.reusable) {
        int count = pool_put(picked, backend_fd);
        if (count > 0) {
            printf("[Pool] %s:%d 연결을 닫지 않고 보관 (보관 중 %d개)\n", dest->host, dest->port, count);
        } else {
            printf("[Pool] %s:%d 보관함이 꽉 차서(%d개) 연결을 닫음\n", dest->host, dest->port, POOL_MAX);
        }
    } else {
        close(backend_fd);
    }

    if (ex.request_failed) {
        free(copy.data);
        free_request(&req);
        send_error(client_fd, 502);
        return 0;
    }
    if (ex.body_failed) {
        free(copy.data);
        free_request(&req);
        return 0;
    }
    /* 서버에서 아무것도 오지 않았으면 502 */
    if (ex.sent == 0) {
        free(copy.data);
        free_request(&req);
        send_error(client_fd, 502);
        return 0;
    }
    printf("응답 %zu바이트를 클라이언트에게 전달\n", ex.sent);

    /* 6) 200 응답이고 사본 하나의 크기 제한 안이면 캐시에 저장한다. 그 밖의 응답(404, 502, 큰 응답)은 저장하지 않는다 */
    if (copy.data != NULL) {
        copy.data[copy.len] = '\0';
        if (copy.too_large) {
            printf("[Cache] 저장 안 함 %s (응답이 %d바이트보다 큼)\n", req.target, CACHE_OBJECT_MAX);
        } else if (!copy.cut && is_cacheable(copy.data, copy.len)) {
            if (cache_put(req.target, copy.data, copy.len, asked_at)) {
                printf("[Cache] 저장 %s %zu바이트 (물어본 때부터 %d초 동안 씀)\n", req.target, copy.len, CACHE_TTL_MS / 1000);
            } else {
                printf("[Cache] 저장 안 함 %s (더 늦게 물어본 사본이 이미 있거나, 받는 동안 만료됨)\n", req.target);
            }
        }
        free(copy.data);
    }
    free_request(&req);
    return ex.kept;
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
