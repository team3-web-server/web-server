#include "cgi.h"

#include "http.h"
#include "net.h"
#include "response.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CGI_OUT_MAX (1024 * 1024) /* CGI 출력 최대 크기(1MiB). 넘으면 500 */
#define ENV_QUERY_SIZE 8300 /* "QUERY_STRING=" + 쿼리 문자열이 들어갈 크기 */

/* CGI 출력에서 헤더와 본문을 나누는 빈 줄("\r\n\r\n" 또는 "\n\n") 다음 위치를 돌려준다. 없으면 -1 */
static long find_cgi_body(const char *out)
{
    const char *crlf = strstr(out, "\r\n\r\n");
    const char *lf = strstr(out, "\n\n");
    if (crlf != NULL && (lf == NULL || crlf < lf)) {
        return (long)(crlf - out) + 4;
    }
    if (lf != NULL) {
        return (long)(lf - out) + 2;
    }
    return -1;
}

int run_cgi(int conn_fd, const char *program_path, const char *method, const char *query, const char *body,
            size_t body_len, int keep_alive)
{
    /* 프로그램 파일이 있고 실행할 수 있는지 확인한다 */
    struct stat st;
    if (stat(program_path, &st) == -1) {
        return send_error(conn_fd, 404);
    }
    if (!S_ISREG(st.st_mode) || access(program_path, X_OK) == -1) {
        return send_error(conn_fd, 403);
    }

    /* CGI 프로그램에 넘길 환경 변수("이름=값")와 argv. 배열의 끝은 NULL */
    char env_method[64];
    char env_query[ENV_QUERY_SIZE];
    char env_length[64];
    snprintf(env_method, sizeof(env_method), "REQUEST_METHOD=%s", method);
    snprintf(env_query, sizeof(env_query), "QUERY_STRING=%s", query);
    snprintf(env_length, sizeof(env_length), "CONTENT_LENGTH=%zu", body_len);
    char *envp[] = {env_method, env_query, env_length, "GATEWAY_INTERFACE=CGI/1.1", "SERVER_PROTOCOL=HTTP/1.1", NULL};

    char *argv[] = {(char *)program_path, NULL};

    /*
     * 파이프 두 개. [1]에 쓴 바이트를 [0]에서 읽는다.
     *   to_child   : 웹 서버 프로세스가 쓰고 → CGI 프로세스가 표준 입력으로 읽는다
     *   from_child : CGI 프로세스가 표준 출력으로 쓰고 → 웹 서버 프로세스가 읽는다
     */
    int to_child[2];
    int from_child[2];
    if (pipe(to_child) == -1) {
        perror("pipe");
        return send_error(conn_fd, 500);
    }
    if (pipe(from_child) == -1) {
        perror("pipe");
        close(to_child[0]);
        close(to_child[1]);
        return send_error(conn_fd, 500);
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return send_error(conn_fd, 500);
    }

    /* 자식 프로세스: 표준 입력(0)과 표준 출력(1)이 파이프를 가리키게 한 뒤 CGI 프로그램을 실행한다 */
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);

        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        close(conn_fd);

        /* main.c에서 무시하게 한 SIGPIPE를 CGI 프로그램에는 기본 동작으로 되돌린다 */
        signal(SIGPIPE, SIG_DFL);

        /* 성공하면 돌아오지 않는다. 돌아왔다면 실패이므로 127로 끝낸다 */
        execve(program_path, argv, envp);
        perror("execve");
        _exit(127);
    }

    /* 부모 프로세스: 쓰지 않는 쪽을 닫는다. from_child[1]을 닫지 않으면 아래 read가 끝(0)을 받지 못한다 */
    close(to_child[0]);
    close(from_child[1]);

    /* POST 본문을 CGI 프로세스의 표준 입력으로 보내고 닫는다. 닫아야 CGI 프로세스의 read가 끝(EOF)을 안다 */
    if (body_len > 0 && write_all(to_child[1], body, body_len) == -1) {
        perror("write (CGI 표준 입력)");
    }
    close(to_child[1]);

    /* CGI 프로세스의 표준 출력을 끝(read가 0)까지 읽는다 */
    char *out = malloc(CGI_OUT_MAX + 1);
    size_t out_len = 0;
    int too_large = 0;
    if (out == NULL) {
        perror("malloc");
    }
    while (out != NULL) {
        if (out_len == CGI_OUT_MAX) {
            too_large = 1;
            break;
        }
        ssize_t n = read(from_child[0], out + out_len, CGI_OUT_MAX - out_len);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("read (CGI 표준 출력)");
            break;
        }
        if (n == 0) {
            break;
        }
        out_len += (size_t)n;
    }
    close(from_child[0]);

    /* CGI 프로세스가 끝나기를 기다리고 종료 값을 받는다. 0이 아니면 500 */
    int wstatus = 0;
    while (waitpid(pid, &wstatus, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        perror("waitpid");
        break;
    }
    int exited_ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
    printf("CGI 프로세스 %d 종료 (출력 %zu바이트, 정상 종료 %s)\n", (int)pid, out_len, exited_ok ? "예" : "아니오");

    if (out == NULL || too_large || !exited_ok) {
        free(out);
        return send_error(conn_fd, 500);
    }
    out[out_len] = '\0';

    /* 출력을 헤더와 본문으로 나누고, 헤더에서 Content-Type을 찾는다 (없으면 text/plain) */
    long body_offset = find_cgi_body(out);
    if (body_offset == -1) {
        free(out);
        return send_error(conn_fd, 500);
    }

    char content_type[128];
    if (find_header(out, "Content-Type", content_type, sizeof(content_type)) != 0) {
        snprintf(content_type, sizeof(content_type), "text/plain; charset=utf-8");
    }

    size_t res_body_len = out_len - (size_t)body_offset;
    char head[256];
    int head_len = snprintf(head, sizeof(head),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            content_type, res_body_len, keep_alive ? "keep-alive" : "close");
    if (write_all(conn_fd, head, (size_t)head_len) == -1 ||
        write_all(conn_fd, out + body_offset, res_body_len) == -1) {
        perror("write");
    }
    free(out);
    return 200;
}
