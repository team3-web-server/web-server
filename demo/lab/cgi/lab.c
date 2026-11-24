/*
 * 데모 페이지(Web Server Lab)의 버튼이 부르는 CGI 프로그램.
 * 웹 서버 프로세스(포트 8000)가 /cgi-bin/lab?action=... 요청을 받으면 이 프로그램을 실행한다.
 * 결과는 JSON으로 표준 출력에 쓴다.
 *
 *   action=send   : 이 프로세스가 클라이언트가 되어 프록시(8080)에 요청을 보내고, 보낸 바이트와 받은 응답을 돌려준다
 *                   mode=reverse&path=/api/         → "GET /api/ HTTP/1.1"
 *                   mode=forward&url=http://...     → "GET http://... HTTP/1.1"
 *   action=status : 프록시와 백엔드 세 개가 살아 있는지
 *   action=stop   : port=8001 처럼 백엔드 하나를 끝낸다
 *   action=start  : 끝낸 백엔드를 다시 실행한다
 *   action=log    : 프록시가 출력한 로그 파일을 from 바이트 위치부터 읽는다
 *   action=conf   : proxy.conf 내용
 *
 * 실행 위치는 레포의 맨 위 폴더다 (demo/lab/run.sh가 거기서 서버를 띄운다).
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PROXY_PORT 8080
#define RUN_DIR "demo/lab/run"
#define PARAM_SIZE 2048
#define RESPONSE_MAX (256 * 1024) /* 저장해 두는 응답의 최대 크기. 넘는 부분은 바이트 수만 센다 */
#define BODY_SHOW_MAX 4000        /* 페이지에 보여 줄 본문의 최대 크기 */
#define LOG_CHUNK_MAX (64 * 1024)

/* 데모가 켜고 끌 수 있는 백엔드. 이 목록 밖의 포트는 건드리지 않는다 */
static const struct {
    int port;
    const char *root;
    const char *route;
} backends[] = {
    {8001, "demo/proxy/backend1", "/api/"},
    {8003, "demo/proxy/backend3", "/api/"},
    {8002, "demo/proxy/backend2", "/static/"},
};
#define BACKEND_COUNT ((int)(sizeof(backends) / sizeof(backends[0])))

/* 16진수 글자 하나를 값으로. 아니면 -1 */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* QUERY_STRING("a=1&b=2")에서 name의 값을 찾아 %XX와 +를 풀어 out에 넣는다. 없으면 -1 */
static int get_param(const char *query, const char *name, char *out, size_t out_size)
{
    size_t name_len = strlen(name);
    const char *p = query;
    while (*p != '\0') {
        const char *end = strchr(p, '&');
        if (end == NULL) {
            end = p + strlen(p);
        }
        if ((size_t)(end - p) > name_len && strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            const char *v = p + name_len + 1;
            size_t n = 0;
            while (v < end && n + 1 < out_size) {
                if (*v == '%' && v + 2 < end && hex_value(v[1]) >= 0 && hex_value(v[2]) >= 0) {
                    out[n++] = (char)(hex_value(v[1]) * 16 + hex_value(v[2]));
                    v += 3;
                } else if (*v == '+') {
                    out[n++] = ' ';
                    v++;
                } else {
                    out[n++] = *v++;
                }
            }
            out[n] = '\0';
            return 0;
        }
        p = *end == '&' ? end + 1 : end;
    }
    return -1;
}

/* s의 len바이트를 JSON 문자열("...")로 출력한다 */
static void json_string(const char *s, size_t len)
{
    putchar('"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            putchar('\\');
            putchar(c);
        } else if (c == '\n') {
            fputs("\\n", stdout);
        } else if (c == '\r') {
            fputs("\\r", stdout);
        } else if (c == '\t') {
            fputs("\\t", stdout);
        } else if (c < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

static void json_header(void)
{
    printf("Content-Type: application/json; charset=utf-8\r\n\r\n");
}

static void json_error(const char *message)
{
    json_header();
    printf("{\"ok\":false,\"error\":");
    json_string(message, strlen(message));
    printf("}\n");
}

/* 127.0.0.1:port에 TCP 연결을 건다. 연결된 소켓 fd, 실패하면 -1 */
static int connect_local(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 그 포트에서 연결을 받는 프로세스가 있는지 (연결해 보고 바로 닫는다) */
static int port_is_up(int port)
{
    int fd = connect_local(port);
    if (fd == -1) {
        return 0;
    }
    close(fd);
    return 1;
}

/* demo/lab/run/<name>.pid 에 적힌 프로세스 번호. 없으면 -1 */
static long read_pid(const char *name)
{
    char path[128];
    snprintf(path, sizeof(path), RUN_DIR "/%s.pid", name);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }
    long pid = -1;
    if (fscanf(fp, "%ld", &pid) != 1) {
        pid = -1;
    }
    fclose(fp);
    return pid;
}

static const char *find_backend_root(int port)
{
    for (int i = 0; i < BACKEND_COUNT; i++) {
        if (backends[i].port == port) {
            return backends[i].root;
        }
    }
    return NULL;
}

/* 0.05초씩 최대 40번(2초) 기다리며 포트 상태가 want(1: 켜짐, 0: 꺼짐)가 되는지 본다 */
static int wait_port(int port, int want)
{
    struct timespec ts = {0, 50 * 1000 * 1000};
    for (int i = 0; i < 40; i++) {
        if (port_is_up(port) == want) {
            return 1;
        }
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* action=send: 프록시에 요청 하나를 보내고 응답을 끝까지 읽는다 */
static void action_send(const char *query)
{
    char mode[32];
    char target[PARAM_SIZE];
    char host[PARAM_SIZE];
    if (get_param(query, "mode", mode, sizeof(mode)) == -1) {
        json_error("mode가 없습니다");
        return;
    }

    if (strcmp(mode, "reverse") == 0) {
        /* Reverse: 요청 줄에 경로만 적는다. 어느 서버로 갈지는 프록시가 정한다 */
        if (get_param(query, "path", target, sizeof(target)) == -1 || target[0] != '/') {
            json_error("path는 /로 시작해야 합니다");
            return;
        }
        snprintf(host, sizeof(host), "localhost:%d", PROXY_PORT);
    } else if (strcmp(mode, "forward") == 0) {
        /* Forward: 요청 줄에 절대 URL을 적는다. 어느 서버로 갈지를 클라이언트가 정한다 */
        if (get_param(query, "url", target, sizeof(target)) == -1 || strncmp(target, "http://", 7) != 0) {
            json_error("url은 http://로 시작해야 합니다");
            return;
        }
        size_t host_len = strcspn(target + 7, "/");
        memcpy(host, target + 7, host_len);
        host[host_len] = '\0';
    } else {
        json_error("mode는 reverse 또는 forward입니다");
        return;
    }
    /* 요청 줄을 깨뜨리는 글자(공백, 줄바꿈)는 받지 않는다 */
    for (const char *c = target; *c != '\0'; c++) {
        if ((unsigned char)*c <= ' ') {
            json_error("요청 대상에 공백이나 줄바꿈을 넣을 수 없습니다");
            return;
        }
    }

    char request[PARAM_SIZE * 2 + 128];
    int request_len = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                               target, host);

    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int fd = connect_local(PROXY_PORT);
    if (fd == -1) {
        json_error("프록시(포트 8080)에 연결하지 못했습니다");
        return;
    }
    /* 프록시가 응답하지 않아도 이 CGI 프로세스가 끝나도록 읽기 제한 시간을 둔다 */
    struct timeval timeout = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (write(fd, request, (size_t)request_len) != request_len) {
        close(fd);
        json_error("프록시에 요청을 쓰지 못했습니다");
        return;
    }

    char *response = malloc(RESPONSE_MAX + 1);
    if (response == NULL) {
        close(fd);
        json_error("메모리가 부족합니다");
        return;
    }
    size_t stored = 0;
    size_t total = 0;
    char discard[4096];
    for (;;) {
        ssize_t n;
        if (stored < RESPONSE_MAX) {
            n = read(fd, response + stored, RESPONSE_MAX - stored);
            if (n > 0) {
                stored += (size_t)n;
            }
        } else {
            n = read(fd, discard, sizeof(discard));
        }
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }
    close(fd);
    response[stored] = '\0';
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0;

    /* 응답을 상태 줄, 헤더, 본문으로 나눈다 */
    int status = 0;
    sscanf(response, "HTTP/%*d.%*d %d", &status);
    size_t line_len = strcspn(response, "\r\n");
    const char *blank = strstr(response, "\r\n\r\n");
    size_t head_len = blank != NULL ? (size_t)(blank - response) : stored;
    const char *body = blank != NULL ? blank + 4 : response + stored;
    size_t body_len = stored - (size_t)(body - response);
    int truncated = body_len > BODY_SHOW_MAX;

    json_header();
    printf("{\"ok\":true,\"mode\":");
    json_string(mode, strlen(mode));
    printf(",\"sent\":");
    json_string(request, (size_t)request_len);
    printf(",\"status\":%d,\"status_line\":", status);
    json_string(response, line_len);
    printf(",\"headers\":");
    json_string(response, head_len);
    printf(",\"body\":");
    json_string(body, truncated ? BODY_SHOW_MAX : body_len);
    printf(",\"body_truncated\":%s,\"bytes\":%zu,\"ms\":%.1f}\n", truncated ? "true" : "false", total, ms);
    free(response);
}

/* action=status: 프록시는 프로세스가 살아 있는지, 백엔드는 포트에 연결되는지로 본다 */
static void action_status(void)
{
    /* 프록시에 연결해 보면 프록시 로그에 "연결 수락"이 찍히므로, 프록시는 프로세스 번호로 확인한다 */
    long proxy_pid = read_pid("proxy");
    int proxy_up = proxy_pid > 0 && kill((pid_t)proxy_pid, 0) == 0;

    json_header();
    printf("{\"ok\":true,\"proxy\":{\"port\":%d,\"up\":%s},\"backends\":[", PROXY_PORT, proxy_up ? "true" : "false");
    for (int i = 0; i < BACKEND_COUNT; i++) {
        printf("%s{\"port\":%d,\"route\":", i > 0 ? "," : "", backends[i].port);
        json_string(backends[i].route, strlen(backends[i].route));
        printf(",\"up\":%s}", port_is_up(backends[i].port) ? "true" : "false");
    }
    printf("]}\n");
}

/* action=stop: 백엔드 프로세스에 SIGTERM을 보낸다 */
static void action_stop(int port)
{
    if (find_backend_root(port) == NULL) {
        json_error("끌 수 있는 포트는 8001, 8002, 8003입니다");
        return;
    }
    char name[16];
    snprintf(name, sizeof(name), "%d", port);
    long pid = read_pid(name);
    if (pid <= 0) {
        json_error("프로세스 번호 파일이 없습니다. demo/lab/run.sh로 실행했는지 확인하세요");
        return;
    }
    kill((pid_t)pid, SIGTERM);
    int down = wait_port(port, 0);
    json_header();
    printf("{\"ok\":true,\"port\":%d,\"up\":%s}\n", port, down ? "false" : "true");
}

/* action=start: 백엔드(웹 서버)를 새 프로세스로 실행한다 */
static void action_start(int port)
{
    const char *root = find_backend_root(port);
    if (root == NULL) {
        json_error("켤 수 있는 포트는 8001, 8002, 8003입니다");
        return;
    }
    if (!port_is_up(port)) {
        char port_text[16];
        char log_path[128];
        char pid_path[128];
        snprintf(port_text, sizeof(port_text), "%d", port);
        snprintf(log_path, sizeof(log_path), RUN_DIR "/%d.log", port);
        snprintf(pid_path, sizeof(pid_path), RUN_DIR "/%d.pid", port);

        fflush(stdout);
        pid_t pid = fork();
        if (pid == -1) {
            json_error("fork에 실패했습니다");
            return;
        }
        if (pid == 0) {
            /*
             * 자식 프로세스: 이 CGI 프로세스가 끝난 뒤에도 계속 실행되어야 한다.
             * 표준 출력이 웹 서버로 가는 파이프를 가리킨 채로 두면 웹 서버의 read가 끝나지 않으므로 로그 파일로 바꾼다.
             */
            setsid();
            int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            int null_fd = open("/dev/null", O_RDONLY);
            if (log_fd == -1 || null_fd == -1) {
                _exit(127);
            }
            dup2(null_fd, STDIN_FILENO);
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            /* 물려받은 다른 fd(웹 서버의 듣기 소켓 등)를 닫는다 */
            for (int fd = 3; fd < 256; fd++) {
                close(fd);
            }
            execl("build/server", "build/server", port_text, root, (char *)NULL);
            _exit(127);
        }
        FILE *fp = fopen(pid_path, "w");
        if (fp != NULL) {
            fprintf(fp, "%d\n", (int)pid);
            fclose(fp);
        }
        wait_port(port, 1);
    }
    json_header();
    printf("{\"ok\":true,\"port\":%d,\"up\":%s}\n", port, port_is_up(port) ? "true" : "false");
}

/* path 파일을 from 바이트 위치부터 최대 max바이트 읽어 {"next":위치,"text":"..."}로 출력한다 */
static void send_file_part(const char *path, long from, size_t max)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        json_error("파일을 열 수 없습니다");
        return;
    }
    struct stat st;
    if (fstat(fd, &st) == -1) {
        close(fd);
        json_error("파일 크기를 알 수 없습니다");
        return;
    }
    /* 파일이 새로 만들어져 전보다 짧아졌으면 처음부터 읽는다 */
    if (from < 0 || from > (long)st.st_size) {
        from = 0;
    }
    char *buf = malloc(max);
    if (buf == NULL) {
        close(fd);
        json_error("메모리가 부족합니다");
        return;
    }
    ssize_t n = pread(fd, buf, max, (off_t)from);
    close(fd);
    if (n < 0) {
        n = 0;
    }
    json_header();
    printf("{\"ok\":true,\"next\":%ld,\"text\":", from + (long)n);
    json_string(buf, (size_t)n);
    printf("}\n");
    free(buf);
}

int main(void)
{
    const char *query = getenv("QUERY_STRING");
    if (query == NULL) {
        query = "";
    }
    char action[32];
    char value[32];
    if (get_param(query, "action", action, sizeof(action)) == -1) {
        json_error("action이 없습니다");
        return 0;
    }

    if (strcmp(action, "send") == 0) {
        action_send(query);
    } else if (strcmp(action, "status") == 0) {
        action_status();
    } else if (strcmp(action, "stop") == 0 || strcmp(action, "start") == 0) {
        int port = 0;
        if (get_param(query, "port", value, sizeof(value)) == 0) {
            port = (int)strtol(value, NULL, 10);
        }
        if (strcmp(action, "stop") == 0) {
            action_stop(port);
        } else {
            action_start(port);
        }
    } else if (strcmp(action, "log") == 0) {
        long from = 0;
        if (get_param(query, "from", value, sizeof(value)) == 0) {
            from = strtol(value, NULL, 10);
        }
        send_file_part(RUN_DIR "/proxy.log", from, LOG_CHUNK_MAX);
    } else if (strcmp(action, "conf") == 0) {
        send_file_part("proxy.conf", 0, LOG_CHUNK_MAX);
    } else {
        json_error("모르는 action입니다");
    }
    return 0;
}
