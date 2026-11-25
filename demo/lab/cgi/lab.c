/*
 * 데모 페이지(Web Server Lab)의 버튼이 부르는 CGI 프로그램.
 * 웹 서버 프로세스(포트 8000)가 /cgi-bin/lab?action=... 요청을 받으면 이 프로그램을 실행한다.
 * 결과는 JSON으로 표준 출력에 쓴다.
 *
 *   action=send   : 이 프로세스가 클라이언트가 되어 to 포트(기본 8080, 프록시)에 raw의 바이트를 그대로 보내고,
 *                   보낸 바이트와 받은 응답을 돌려준다. 요청 줄과 헤더는 페이지(lab.js)가 적어 준다
 *   action=status : 프록시와 백엔드들이 살아 있는지
 *   action=stop   : port=8001 처럼 백엔드 하나를 끝낸다
 *   action=start  : 끝낸 백엔드를 다시 실행한다
 *   action=multi  : 요청 세 개를 연결마다 하나씩(keep=0) 또는 연결 하나로(keep=1, keep-alive) 보낸다
 *   action=fds    : 웹 서버 프로세스(부모와 자식)의 fd 표를 /proc 에서 읽는다 (연결 전, 연결된 때, 끝난 뒤)
 *   action=log    : name(proxy, tcpdump 또는 백엔드 포트)의 프로세스가 출력한 로그 파일을 from 바이트 위치부터 읽는다
 *   action=conf   : 데모용 라우팅 규칙 파일(demo/lab/proxy.conf) 내용
 *
 * 실행 위치는 레포의 맨 위 폴더다 (demo/lab/run.sh가 거기서 서버를 띄운다).
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
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
#define SENT_SHOW_MAX 1500        /* 페이지에 보여 줄 보낸 요청의 최대 크기 */
#define QUERY_MAX 8400            /* QUERY_STRING에서 꺼내는 raw의 최대 크기 (웹 서버의 요청 버퍼가 8192바이트) */
#define REQUEST_MAX (2 * 1024 * 1024) /* {{xN}}을 펼친 뒤 요청의 최대 크기 */
#define LOG_CHUNK_MAX (64 * 1024)

/* 데모가 켜고 끌 수 있는 백엔드. 이 목록 밖의 포트는 건드리지 않는다 */
static const struct {
    int port;
    const char *root;
    const char *route;
    const char *bin;
} backends[] = {
    {8001, "demo/proxy/backend1", "/api/", "build/server"},
    {8003, "demo/proxy/backend3", "/api/", "build/server"},
    {8002, "demo/proxy/backend2", "/static/", "build/server"},
    {8004, "www", "/cgi-bin/", "build/server"}, /* CGI 프로그램 hello, echo가 있는 폴더 */
    /* 같은 코드를 -DNO_FORK로 빌드한 웹 서버: 연결을 자식 프로세스에게 맡기지 않고 직접 처리한다. 프록시 규칙에는 없다 */
    {8005, "www", "", "build/server-nofork"},
    /* 같은 코드를 -DNO_THREAD로 빌드한 프록시: 연결마다 스레드를 만들지 않는다. 둘째 인자는 폴더가 아니라 설정 파일이다 */
    {8081, "demo/lab/proxy.conf", "", "build/proxy-nothread"},
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

static const char *find_backend_bin(int port)
{
    for (int i = 0; i < BACKEND_COUNT; i++) {
        if (backends[i].port == port) {
            return backends[i].bin;
        }
    }
    return NULL;
}

static int backend_is_up(int port);

/* 0.05초씩 최대 40번(2초) 기다리며 백엔드 프로세스의 상태가 want(1: 실행 중, 0: 끝남)가 되는지 본다 */
static int wait_backend(int port, int want)
{
    struct timespec ts = {0, 50 * 1000 * 1000};
    for (int i = 0; i < 40; i++) {
        if (backend_is_up(port) == want) {
            return 1;
        }
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* 데모가 요청을 보낼 수 있는 포트인지 (프록시와 백엔드들) */
static int is_allowed_port(int port)
{
    return port == PROXY_PORT || find_backend_root(port) != NULL;
}

/*
 * raw 안의 "{{xN}}"을 글자 x를 N번 반복한 것으로 바꿔 out에 쓴다. 쓴 바이트 수, 넘치면 -1.
 * 아주 긴 경로나 헤더(414, 431을 내는 요청)를 주소 창 길이 제한 없이 만들기 위한 표기다.
 */
static long expand_repeats(const char *raw, char *out, size_t out_size)
{
    size_t n = 0;
    const char *p = raw;
    while (*p != '\0') {
        char c;
        long count;
        int used = 0;
        if (p[0] == '{' && p[1] == '{' && sscanf(p + 2, "%c%ld}}%n", &c, &count, &used) == 2 && used > 0) {
            if (count < 0 || n + (size_t)count >= out_size) {
                return -1;
            }
            memset(out + n, c, (size_t)count);
            n += (size_t)count;
            p += 2 + used;
            continue;
        }
        if (n + 1 >= out_size) {
            return -1;
        }
        out[n++] = *p++;
    }
    return (long)n;
}

/*
 * action=send: raw에 적힌 바이트를 to 포트에 그대로 보내고, 응답을 끝까지 읽어 돌려준다.
 * 요청 줄과 헤더를 어떻게 적을지는 페이지(lab.js)가 정한다. 이 프로그램은 적힌 대로 보내기만 한다.
 */
static void action_send(const char *query)
{
    char value[32];
    int to = PROXY_PORT;
    if (get_param(query, "to", value, sizeof(value)) == 0) {
        to = (int)strtol(value, NULL, 10);
    }
    if (!is_allowed_port(to)) {
        json_error("to는 프록시(8080)나 백엔드 포트여야 합니다");
        return;
    }

    char *raw = malloc(QUERY_MAX);
    char *request = malloc(REQUEST_MAX);
    char *response = malloc(RESPONSE_MAX + 1);
    if (raw == NULL || request == NULL || response == NULL) {
        json_error("메모리가 부족합니다");
        return;
    }
    if (get_param(query, "raw", raw, QUERY_MAX) == -1 || raw[0] == '\0') {
        json_error("raw가 없습니다");
        return;
    }
    long request_len = expand_repeats(raw, request, REQUEST_MAX);
    if (request_len == -1) {
        json_error("요청이 너무 깁니다");
        return;
    }

    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int fd = connect_local(to);
    if (fd == -1) {
        json_header();
        printf("{\"ok\":false,\"refused\":true,\"to\":%d,\"error\":\"포트 %d에 연결하지 못했습니다 (connect 실패)\"}\n", to, to);
        return;
    }
    /* 상대가 응답하지 않아도 이 CGI 프로세스가 끝나도록 읽기 제한 시간을 둔다 */
    struct timeval timeout = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /* 상대가 요청을 다 읽지 않고 응답한 뒤 닫아도(예: 431) 이 프로세스가 SIGPIPE로 죽지 않게 한다 */
    signal(SIGPIPE, SIG_IGN);
    long written = 0;
    while (written < request_len) {
        ssize_t n = write(fd, request + written, (size_t)(request_len - written));
        if (n <= 0) {
            break;
        }
        written += n;
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
    const char *res_body = blank != NULL ? blank + 4 : response + stored;
    size_t res_body_len = stored - (size_t)(res_body - response);
    int truncated = res_body_len > BODY_SHOW_MAX;

    json_header();
    printf("{\"ok\":true,\"to\":%d,\"sent_bytes\":%ld,\"written_bytes\":%ld,\"sent\":", to, request_len, written);
    json_string(request, request_len > SENT_SHOW_MAX ? SENT_SHOW_MAX : (size_t)request_len);
    printf(",\"sent_truncated\":%s", request_len > SENT_SHOW_MAX ? "true" : "false");
    printf(",\"status\":%d,\"status_line\":", status);
    json_string(response, line_len);
    printf(",\"headers\":");
    json_string(response, head_len);
    printf(",\"body\":");
    json_string(res_body, truncated ? BODY_SHOW_MAX : res_body_len);
    printf(",\"body_truncated\":%s,\"bytes\":%zu,\"ms\":%.1f}\n", truncated ? "true" : "false", total, ms);
    free(raw);
    free(request);
    free(response);
}

/*
 * 이름(proxy, 8001, ...)의 프로세스가 실행 중인지. 프로세스 번호 파일의 번호로 /proc/번호/stat 을 읽는다.
 * 포트에 연결해 보는 방법은 그 서버의 로그에 "연결 수락"을 남기므로, 화면에 보여 주는 로그를 흐리지 않게 이 방법을 쓴다.
 * 끝났지만 부모가 아직 거두지 않은 프로세스(상태 Z)는 꺼진 것으로 본다.
 */
static int process_is_up(const char *name)
{
    long pid = read_pid(name);
    if (pid <= 0) {
        return 0;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }
    /* 형식: "번호 (프로그램 이름) 상태 ..." 상태 글자는 마지막 ')' 다음에 있다 */
    char line[512];
    char state = 'Z';
    if (fgets(line, sizeof(line), fp) != NULL) {
        const char *close_paren = strrchr(line, ')');
        if (close_paren != NULL && close_paren[1] == ' ') {
            state = close_paren[2];
        }
    }
    fclose(fp);
    return state != 'Z' && state != 'X';
}

static int backend_is_up(int port)
{
    char name[16];
    snprintf(name, sizeof(name), "%d", port);
    return process_is_up(name);
}

/* action=status: 프록시와 백엔드 프로세스가 실행 중인지 */
static void action_status(void)
{
    json_header();
    printf("{\"ok\":true,\"capture\":%s,\"proxy\":{\"port\":%d,\"up\":%s},\"backends\":[",
           process_is_up("tcpdump") ? "true" : "false", PROXY_PORT, process_is_up("proxy") ? "true" : "false");
    for (int i = 0; i < BACKEND_COUNT; i++) {
        printf("%s{\"port\":%d,\"route\":", i > 0 ? "," : "", backends[i].port);
        json_string(backends[i].route, strlen(backends[i].route));
        printf(",\"up\":%s}", backend_is_up(backends[i].port) ? "true" : "false");
    }
    printf("]}\n");
}

/*
 * 부모가 parent인 프로세스(백엔드가 연결마다 fork한 자식 프로세스)를 모두 끝낸다.
 * /proc/<번호>/stat 의 넷째 칸이 부모의 프로세스 번호다. 둘째 칸(프로그램 이름)은 괄호 안이고 공백이 있을 수 있어서 마지막 ')' 뒤부터 읽는다.
 */
static void kill_children(long parent)
{
    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end != '\0' || pid <= 0) {
            continue;
        }
        char path[64];
        char line[512];
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            continue;
        }
        long ppid = -1;
        if (fgets(line, sizeof(line), fp) != NULL) {
            const char *close_paren = strrchr(line, ')');
            if (close_paren != NULL) {
                sscanf(close_paren + 1, " %*c %ld", &ppid);
            }
        }
        fclose(fp);
        if (ppid == parent) {
            kill((pid_t)pid, SIGKILL);
        }
    }
    closedir(dir);
}

/*
 * action=stop: 백엔드 프로세스에 SIGTERM을 보내고, 그 자식 프로세스들도 끝낸다.
 * 백엔드는 SIGTERM을 받아도 맡고 있던 연결의 자식 프로세스가 끝나기를 기다린다. 프록시가 보관한 연결(keep-alive)이 있으면
 * 그 자식 프로세스가 최대 5초 더 남아 요청을 처리하므로, "백엔드가 꺼졌다"는 장면이 되도록 함께 끝낸다.
 */
static void action_stop(int port)
{
    if (find_backend_root(port) == NULL) {
        json_error("끌 수 있는 포트는 8001~8005입니다");
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
    kill_children(pid);
    int down = wait_backend(port, 0);
    json_header();
    printf("{\"ok\":true,\"port\":%d,\"up\":%s}\n", port, down ? "false" : "true");
}

/* action=start: 백엔드(웹 서버)를 새 프로세스로 실행한다 */
static void action_start(int port)
{
    const char *root = find_backend_root(port);
    if (root == NULL) {
        json_error("켤 수 있는 포트는 8001~8005입니다");
        return;
    }
    if (!backend_is_up(port)) {
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
            execl(find_backend_bin(port), find_backend_bin(port), port_text, root, (char *)NULL);
            _exit(127);
        }
        FILE *fp = fopen(pid_path, "w");
        if (fp != NULL) {
            fprintf(fp, "%d\n", (int)pid);
            fclose(fp);
        }
        /* 새 프로세스가 bind, listen을 마칠 시간을 준다 */
        struct timespec ts = {0, 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    json_header();
    printf("{\"ok\":true,\"port\":%d,\"up\":%s}\n", port, backend_is_up(port) ? "true" : "false");
}


/* ---------- action=procs: 프로세스 표 (좀비 프로세스가 남는지, 스레드가 몇 개인지) ---------- */

static void print_fd_table(long pid);

/* /proc/<pid>/stat 에서 상태 글자와 프로그램 이름을 읽는다. 읽지 못하면 0 */
static int read_proc_state(long pid, char *state, char *comm, size_t comm_size)
{
    char path[64];
    char line[512];
    snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }
    int ok = 0;
    if (fgets(line, sizeof(line), fp) != NULL) {
        char *open_paren = strchr(line, '(');
        char *close_paren = strrchr(line, ')');
        if (open_paren != NULL && close_paren != NULL && close_paren > open_paren && close_paren[1] == ' ') {
            *close_paren = '\0';
            snprintf(comm, comm_size, "%s", open_paren + 1);
            *state = close_paren[2];
            ok = 1;
        }
    }
    fclose(fp);
    return ok;
}

/*
 * 부모가 parent인 프로세스를 모두 적는다. find_children과 달리 좀비 프로세스(상태 Z)도 넣는다:
 * 끝났지만 부모가 waitpid로 거두지 않은 프로세스는 /proc 에 상태 Z로 남고, 부모가 거두면 /proc 에서 사라진다.
 * depth가 남아 있으면 그 자식들(웹 서버의 자식 프로세스가 실행한 CGI 프로세스)도 적는다.
 */
static void print_children(long parent, int depth)
{
    printf("[");
    DIR *dir = opendir("/proc");
    int count = 0;
    struct dirent *entry;
    while (dir != NULL && (entry = readdir(dir)) != NULL) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end != '\0' || pid <= 0) {
            continue;
        }
        char path[64];
        char line[512];
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            continue;
        }
        long ppid = -1;
        if (fgets(line, sizeof(line), fp) != NULL) {
            const char *close_paren = strrchr(line, ')');
            if (close_paren != NULL) {
                sscanf(close_paren + 1, " %*c %ld", &ppid);
            }
        }
        fclose(fp);
        if (ppid != parent) {
            continue;
        }
        char state = '?';
        char comm[64] = "";
        if (!read_proc_state(pid, &state, comm, sizeof(comm))) {
            continue;
        }
        printf("%s{\"pid\":%ld,\"state\":\"%c\",\"comm\":", count++ > 0 ? "," : "", pid, state);
        json_string(comm, strlen(comm));
        if (depth > 0) {
            printf(",\"children\":");
            print_children(pid, depth - 1);
        }
        printf("}");
    }
    if (dir != NULL) {
        closedir(dir);
    }
    printf("]");
}

/*
 * action=procs: 이름(백엔드 포트 번호 또는 proxy)의 프로세스에 대해
 *   children: 그 프로세스의 자식 프로세스와 손자 프로세스 (번호, 상태 글자, 프로그램 이름)
 *   threads : 그 프로세스 안의 스레드 (/proc/<pid>/task 의 항목. 스레드 번호가 프로세스 번호와 같은 것이 main 스레드)
 */
static void action_procs(const char *name)
{
    long pid = read_pid(name);
    char state = '?';
    char comm[64] = "";
    if (pid <= 0 || !read_proc_state(pid, &state, comm, sizeof(comm))) {
        json_header();
        printf("{\"ok\":true,\"up\":false}\n");
        return;
    }
    json_header();
    printf("{\"ok\":true,\"up\":true,\"pid\":%ld,\"state\":\"%c\",\"comm\":", pid, state);
    json_string(comm, strlen(comm));
    printf(",\"children\":");
    print_children(pid, 1);
    printf(",\"threads\":[");
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/task", pid);
    DIR *dir = opendir(path);
    int count = 0;
    struct dirent *entry;
    while (dir != NULL && (entry = readdir(dir)) != NULL) {
        char *end;
        long tid = strtol(entry->d_name, &end, 10);
        if (*end != '\0' || tid <= 0) {
            continue;
        }
        printf("%s%ld", count++ > 0 ? "," : "", tid);
    }
    if (dir != NULL) {
        closedir(dir);
    }
    printf("]");
    /* fds: 그 프로세스의 fd 표. 프록시라면 클라이언트와의 연결, 백엔드와의 연결(보관 중인 것 포함)이 소켓으로 보인다 */
    printf(",\"fds\":");
    print_fd_table(pid);
    printf("}\n");
}

/* ---------- action=fds: 웹 서버 프로세스의 fd 표를 /proc 에서 읽는다 ---------- */

/*
 * 소켓 번호(inode)로 /proc/net/tcp 에서 그 소켓의 상태와 양쪽 주소를 찾아 out에 쓴다.
 * /proc/net/tcp 는 커널이 가진 TCP 소켓 목록이다. 상태 0A는 LISTEN, 01은 ESTABLISHED.
 */
static void describe_socket(unsigned long inode, char *out, size_t out_size)
{
    out[0] = '\0';
    FILE *fp = fopen("/proc/net/tcp", "r");
    if (fp == NULL) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned int laddr, lport, raddr, rport, state;
        unsigned long ino;
        if (sscanf(line, "%*d: %x:%x %x:%x %x %*s %*s %*s %*d %*d %lu", &laddr, &lport, &raddr, &rport, &state, &ino) != 6 ||
            ino != inode) {
            continue;
        }
        struct in_addr la = {laddr};
        struct in_addr ra = {raddr};
        char ltext[32];
        char rtext[32];
        snprintf(ltext, sizeof(ltext), "%s", inet_ntoa(la));
        snprintf(rtext, sizeof(rtext), "%s", inet_ntoa(ra));
        if (state == 0x0A) {
            snprintf(out, out_size, "LISTEN %s:%u", ltext, lport);
        } else if (state == 0x01) {
            snprintf(out, out_size, "ESTABLISHED %s:%u <-> %s:%u", ltext, lport, rtext, rport);
        } else {
            snprintf(out, out_size, "상태 %02X %s:%u <-> %s:%u", state, ltext, lport, rtext, rport);
        }
        break;
    }
    fclose(fp);
}

/* 프로세스 pid의 fd 표(/proc/pid/fd)를 JSON 배열로 출력한다: [{"fd":3,"target":"socket:[123]","sock":"LISTEN ..."}] */
static void print_fd_table(long pid)
{
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "/proc/%ld/fd", pid);
    printf("[");
    DIR *dir = opendir(dir_path);
    if (dir != NULL) {
        int numbers[256];
        int count = 0;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && count < 256) {
            if (isdigit((unsigned char)entry->d_name[0])) {
                numbers[count++] = (int)strtol(entry->d_name, NULL, 10);
            }
        }
        closedir(dir);
        /* readdir의 순서는 정해져 있지 않으므로 번호 순으로 놓는다 */
        for (int i = 1; i < count; i++) {
            for (int j = i; j > 0 && numbers[j - 1] > numbers[j]; j--) {
                int tmp = numbers[j];
                numbers[j] = numbers[j - 1];
                numbers[j - 1] = tmp;
            }
        }
        int printed = 0;
        for (int i = 0; i < count; i++) {
            char link_path[96];
            char target[256];
            snprintf(link_path, sizeof(link_path), "/proc/%ld/fd/%d", pid, numbers[i]);
            ssize_t n = readlink(link_path, target, sizeof(target) - 1);
            if (n < 0) {
                continue;
            }
            target[n] = '\0';
            char sock[128] = "";
            unsigned long inode;
            if (sscanf(target, "socket:[%lu]", &inode) == 1) {
                describe_socket(inode, sock, sizeof(sock));
            }
            printf("%s{\"fd\":%d,\"target\":", printed++ > 0 ? "," : "", numbers[i]);
            json_string(target, strlen(target));
            printf(",\"sock\":");
            json_string(sock, strlen(sock));
            printf("}");
        }
    }
    printf("]");
}

/* 부모가 parent인 프로세스들을 /proc 에서 찾아 pids에 넣고 개수를 돌려준다 */
static int find_children(long parent, long *pids, int max)
{
    int count = 0;
    DIR *dir = opendir("/proc");
    if (dir == NULL) {
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max) {
        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }
        char path[300];
        snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            continue;
        }
        /* 형식: "번호 (프로그램 이름) 상태 부모번호 ..." */
        char line[512];
        if (fgets(line, sizeof(line), fp) != NULL) {
            const char *close_paren = strrchr(line, ')');
            char state;
            long ppid;
            if (close_paren != NULL && sscanf(close_paren + 1, " %c %ld", &state, &ppid) == 2 && ppid == parent &&
                state != 'Z') {
                pids[count++] = strtol(entry->d_name, NULL, 10);
            }
        }
        fclose(fp);
    }
    closedir(dir);
    return count;
}

/* 한 시점의 모습: 웹 서버 프로세스(부모)와 그 자식 프로세스들의 fd 표 */
static void print_snapshot(const char *name, long pid)
{
    printf("{\"name\":\"%s\",\"procs\":[{\"pid\":%ld,\"role\":\"parent\",\"fds\":", name, pid);
    print_fd_table(pid);
    printf("}");
    long children[16];
    int count = find_children(pid, children, 16);
    for (int i = 0; i < count; i++) {
        printf(",{\"pid\":%ld,\"role\":\"child\",\"fds\":", children[i]);
        print_fd_table(children[i]);
        printf("}");
        /* 자식 프로세스가 fork, exec로 실행한 CGI 프로세스 */
        long grand[4];
        int grand_count = find_children(children[i], grand, 4);
        for (int j = 0; j < grand_count; j++) {
            printf(",{\"pid\":%ld,\"role\":\"cgi\",\"fds\":", grand[j]);
            print_fd_table(grand[j]);
            printf("}");
        }
    }
    printf("]}");
}

/*
 * action=fds: 웹 서버(port)의 fd 표를 세 시점에 읽는다.
 *   before    : 연결하기 전
 *   connected : 이 프로세스가 connect만 하고 요청은 아직 보내지 않은 때 (자식 프로세스가 recv에서 기다리는 중)
 *   after     : 요청을 보내고 응답을 다 받은 뒤
 * cgi가 1이면 두 번째 시점이 다르다. 3초 걸리는 CGI 요청(/cgi-bin/slow)을 먼저 보내고, CGI 프로세스가 실행 중인 때에 읽는다.
 */
static void action_fds(int port, int cgi)
{
    if (find_backend_root(port) == NULL) {
        json_error("port는 백엔드 포트여야 합니다");
        return;
    }
    char name[16];
    snprintf(name, sizeof(name), "%d", port);
    long pid = read_pid(name);
    if (pid <= 0 || !backend_is_up(port)) {
        json_error("그 포트의 웹 서버 프로세스가 실행 중이 아닙니다");
        return;
    }
    struct timespec pause = {0, 150 * 1000 * 1000};

    json_header();
    printf("{\"ok\":true,\"port\":%d,\"pid\":%ld,\"steps\":[", port, pid);
    print_snapshot("before", pid);

    int fd = connect_local(port);
    if (fd == -1) {
        printf("],\"error\":\"connect 실패\"}\n");
        return;
    }
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    getsockname(fd, (struct sockaddr *)&local, &local_len);
    const char *request = cgi ? "GET /cgi-bin/slow HTTP/1.0\r\n\r\n" : "GET /index.html HTTP/1.0\r\n\r\n";
    struct timeval timeout = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    signal(SIGPIPE, SIG_IGN);
    ssize_t written = 0;
    if (cgi) {
        written = write(fd, request, strlen(request));
    }
    /* 웹 서버 프로세스가 accept, fork를 (cgi면 CGI 프로세스 실행까지) 마칠 시간을 준다 */
    nanosleep(&pause, NULL);
    printf(",");
    print_snapshot(cgi ? "running" : "connected", pid);
    if (!cgi) {
        written = write(fd, request, strlen(request));
    }
    char buf[4096];
    char first[128] = "";
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (total == 0) {
            size_t line_len = 0;
            while (line_len < (size_t)n && line_len < sizeof(first) - 1 && buf[line_len] != '\r' && buf[line_len] != '\n') {
                line_len++;
            }
            memcpy(first, buf, line_len);
            first[line_len] = '\0';
        }
        total += (size_t)n;
    }
    close(fd);
    nanosleep(&pause, NULL);
    printf(",");
    print_snapshot("after", pid);

    int status = 0;
    sscanf(first, "HTTP/%*d.%*d %d", &status);
    printf("],\"client_fd\":%d,\"client_port\":%d,\"sent\":", fd, ntohs(local.sin_port));
    json_string(request, written > 0 ? (size_t)written : 0);
    printf(",\"status\":%d,\"status_line\":", status);
    json_string(first, strlen(first));
    printf(",\"bytes\":%zu}\n", total);
}


/* ---------- action=multi: 요청 세 개를 연결마다 하나씩(HTTP/1.0) 또는 연결 하나로(HTTP/1.1 keep-alive) 보낸다 ---------- */

/*
 * fd에서 응답 하나를 읽는다. 헤더의 Content-Length만큼 본문을 읽으면 멈춘다 (연결이 닫히기를 기다리지 않는다).
 * 상태 줄을 first에 복사하고, 읽은 바이트 수를 돌려준다. 실패하면 -1.
 */
static long read_one_response(int fd, char *first, size_t first_size)
{
    char buf[RESPONSE_MAX];
    size_t total = 0;
    char *blank = NULL;
    while (blank == NULL && total < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
        buf[total] = '\0';
        blank = strstr(buf, "\r\n\r\n");
    }
    if (blank == NULL) {
        return -1;
    }
    size_t line_len = strcspn(buf, "\r\n");
    snprintf(first, first_size, "%.*s", (int)line_len, buf);

    long content_length = 0;
    const char *field = strstr(buf, "Content-Length:");
    if (field != NULL && field < blank) {
        content_length = strtol(field + strlen("Content-Length:"), NULL, 10);
    }
    size_t want = (size_t)(blank + 4 - buf) + (size_t)content_length;
    while (total < want) {
        char rest[4096];
        size_t left = want - total;
        ssize_t n = read(fd, rest, left < sizeof(rest) ? left : sizeof(rest));
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return (long)total;
}

static double elapsed_ms(const struct timespec *from)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - from->tv_sec) * 1000.0 + (double)(now.tv_nsec - from->tv_nsec) / 1000000.0;
}

static int local_port(int fd)
{
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &local_len) == -1) {
        return 0;
    }
    return ntohs(local.sin_port);
}

/*
 * keep가 0이면: 요청마다 connect → HTTP/1.0 요청 → 응답 → 서버가 닫음 (연결 3개)
 * keep가 1이면: connect 한 번 → HTTP/1.1 요청 세 개를 같은 소켓으로 (마지막 요청에 Connection: close)
 * 요청마다 이 프로세스 쪽 소켓의 fd와 포트를 함께 돌려준다. 포트가 같으면 같은 연결이다.
 */
static void action_multi(int port, int keep)
{
    /* 프록시(8080)로 보낼 때는 Round-Robin 규칙이 있는 /api/ 를 세 번 요청한다 */
    static const char *const direct_paths[] = {"/index.html", "/style.css", "/app.js"};
    static const char *const proxy_paths[] = {"/api/", "/api/", "/api/"};
    const char *const *paths = port == PROXY_PORT ? proxy_paths : direct_paths;
    const int count = 3;
    if (!is_allowed_port(port)) {
        json_error("port는 프록시 또는 백엔드 포트여야 합니다");
        return;
    }
    signal(SIGPIPE, SIG_IGN);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    json_header();
    printf("{\"ok\":true,\"port\":%d,\"keep\":%s,\"requests\":[", port, keep ? "true" : "false");
    int fd = -1;
    int connections = 0;
    for (int i = 0; i < count; i++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int connected_now = 0;
        if (fd == -1) {
            fd = connect_local(port);
            if (fd == -1) {
                printf("],\"error\":\"connect 실패\"}\n");
                return;
            }
            struct timeval timeout = {10, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            connections++;
            connected_now = 1;
        }
        char request[256];
        int request_len;
        if (keep) {
            request_len = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: localhost\r\n%s\r\n", paths[i],
                                   i == count - 1 ? "Connection: close\r\n" : "");
        } else {
            request_len = snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\n\r\n", paths[i]);
        }
        int client_fd = fd;
        int client_port = local_port(fd);
        char first[128] = "";
        long bytes = -1;
        if (write(fd, request, (size_t)request_len) == request_len) {
            bytes = read_one_response(fd, first, sizeof(first));
        }
        double ms = elapsed_ms(&t0);
        if (!keep || i == count - 1 || bytes == -1) {
            close(fd);
            fd = -1;
        }
        printf("%s{\"sent\":", i > 0 ? "," : "");
        json_string(request, (size_t)request_len);
        printf(",\"status_line\":");
        json_string(first, strlen(first));
        printf(",\"bytes\":%ld,\"ms\":%.2f,\"client_fd\":%d,\"client_port\":%d,\"connected_now\":%s}", bytes, ms, client_fd,
               client_port, connected_now ? "true" : "false");
        if (bytes == -1) {
            break;
        }
    }
    printf("],\"connections\":%d,\"total_ms\":%.2f}\n", connections, elapsed_ms(&start));
}

/*
 * action=burst: 프록시(8080)에 연결 n개를 먼저 맺고, 그 연결들에 요청을 한꺼번에 쓴 뒤 응답을 차례로 읽는다.
 * 프록시의 연결 스레드 n개가 같은 때에 백엔드와의 연결을 필요로 하게 되어, 보관함에 여러 연결이 들어가는 것을 볼 수 있다.
 * slow가 1이면 3초 걸리는 /cgi-bin/slow 를 요청한다(모든 요청이 확실히 겹친다). 아니면 /api/ 를 요청한다.
 */
#define BURST_MAX 16
static void action_burst(int n, int slow)
{
    if (n < 1 || n > BURST_MAX) {
        json_error("n은 1~16입니다");
        return;
    }
    signal(SIGPIPE, SIG_IGN);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const char *request = slow ? "GET /cgi-bin/slow HTTP/1.0\r\n\r\n" : "GET /api/ HTTP/1.0\r\n\r\n";
    size_t request_len = strlen(request);

    int fds[BURST_MAX];
    for (int i = 0; i < n; i++) {
        fds[i] = connect_local(PROXY_PORT);
        if (fds[i] != -1) {
            struct timeval timeout = {10, 0};
            setsockopt(fds[i], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        }
    }
    for (int i = 0; i < n; i++) {
        if (fds[i] != -1 && write(fds[i], request, request_len) != (ssize_t)request_len) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
    json_header();
    printf("{\"ok\":true,\"n\":%d,\"sent\":", n);
    json_string(request, request_len);
    printf(",\"requests\":[");
    int good = 0;
    for (int i = 0; i < n; i++) {
        char first[128] = "";
        long bytes = -1;
        int port = -1;
        if (fds[i] != -1) {
            port = local_port(fds[i]);
            bytes = read_one_response(fds[i], first, sizeof(first));
            close(fds[i]);
        }
        if (strstr(first, " 200 ") != NULL) {
            good++;
        }
        printf("%s{\"status_line\":", i > 0 ? "," : "");
        json_string(first, strlen(first));
        printf(",\"bytes\":%ld,\"client_port\":%d}", bytes, port);
    }
    printf("],\"good\":%d,\"total_ms\":%.2f}\n", good, elapsed_ms(&start));
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
    /* from이 음수면 파일 끝에서 그만큼 앞부터(마지막 부분만) 읽는다. 파일이 전보다 짧아졌으면 처음부터 읽는다 */
    if (from < 0) {
        from = (long)st.st_size + from;
        if (from < 0) {
            from = 0;
        }
    }
    if (from > (long)st.st_size) {
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
    } else if (strcmp(action, "multi") == 0) {
        int port = 8004;
        if (get_param(query, "port", value, sizeof(value)) == 0) {
            port = (int)strtol(value, NULL, 10);
        }
        int keep = get_param(query, "keep", value, sizeof(value)) == 0 && strcmp(value, "1") == 0;
        action_multi(port, keep);
    } else if (strcmp(action, "burst") == 0) {
        int n = 8;
        if (get_param(query, "n", value, sizeof(value)) == 0) {
            n = (int)strtol(value, NULL, 10);
        }
        int slow = get_param(query, "slow", value, sizeof(value)) == 0 && strcmp(value, "1") == 0;
        action_burst(n, slow);
    } else if (strcmp(action, "fds") == 0) {
        int port = 8004;
        if (get_param(query, "port", value, sizeof(value)) == 0) {
            port = (int)strtol(value, NULL, 10);
        }
        int cgi = get_param(query, "mode", value, sizeof(value)) == 0 && strcmp(value, "cgi") == 0;
        action_fds(port, cgi);
    } else if (strcmp(action, "log") == 0) {
        long from = 0;
        if (get_param(query, "from", value, sizeof(value)) == 0) {
            from = strtol(value, NULL, 10);
        }
        /* name: proxy 또는 백엔드 포트 번호. 그 프로세스의 출력이 쌓이는 파일을 읽는다 */
        char name[16];
        char path[128];
        if (get_param(query, "name", name, sizeof(name)) == -1) {
            snprintf(name, sizeof(name), "proxy");
        }
        /* tcpdump: 패킷 캡처 프로그램(tcpdump)이 포트 8004의 패킷을 한 줄씩 적는 파일 */
        int is_named = strcmp(name, "proxy") == 0 || strcmp(name, "tcpdump") == 0;
        if (!is_named && find_backend_root((int)strtol(name, NULL, 10)) == NULL) {
            json_error("name은 proxy, tcpdump 또는 백엔드 포트 번호입니다");
            return 0;
        }
        if (!is_named) {
            snprintf(name, sizeof(name), "%d", (int)strtol(name, NULL, 10));
        }
        snprintf(path, sizeof(path), RUN_DIR "/%s.log", name);
        send_file_part(path, from, LOG_CHUNK_MAX);
    } else if (strcmp(action, "procs") == 0) {
        /* name: proxy 또는 백엔드 포트 번호 (log와 같은 규칙) */
        char name[16];
        if (get_param(query, "name", name, sizeof(name)) == -1) {
            snprintf(name, sizeof(name), "8004");
        }
        if (strcmp(name, "proxy") != 0) {
            int port = (int)strtol(name, NULL, 10);
            if (find_backend_root(port) == NULL) {
                json_error("name은 proxy 또는 백엔드 포트 번호입니다");
                return 0;
            }
            snprintf(name, sizeof(name), "%d", port);
        }
        action_procs(name);
    } else if (strcmp(action, "conf") == 0) {
        send_file_part("demo/lab/proxy.conf", 0, LOG_CHUNK_MAX);
    } else {
        json_error("모르는 action입니다");
    }
    return 0;
}
