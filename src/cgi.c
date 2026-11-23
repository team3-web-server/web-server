#include "cgi.h"

#include "http.h"
#include "net.h"
#include "response.h"

#include <errno.h>     /* errno, EINTR */
#include <signal.h>    /* signal, SIGPIPE, SIG_DFL */
#include <stdio.h>     /* snprintf, perror, printf */
#include <stdlib.h>    /* malloc, free */
#include <string.h>    /* strstr */
#include <sys/stat.h>  /* stat, struct stat, S_ISREG */
#include <sys/types.h> /* pid_t, ssize_t */
#include <sys/wait.h>  /* waitpid, WIFEXITED, WEXITSTATUS */
#include <unistd.h>    /* access, pipe, fork, dup2, execve, read, write, close, _exit */

#define CGI_OUT_MAX (1024 * 1024) /* CGI 출력을 받는 버퍼 크기: 1MiB. 넘으면 500 */
#define ENV_QUERY_SIZE 8300       /* "QUERY_STRING=" + 쿼리 문자열. 쿼리 문자열은 요청 버퍼(8192)보다 길 수 없다 */

/*
 * CGI 출력(out, 길이 len)에서 헤더와 본문을 나누는 빈 줄을 찾는다.
 * 줄 끝은 "\r\n"일 수도 "\n"일 수도 있으므로 "\r\n\r\n"과 "\n\n" 중 먼저 나오는 것을 쓴다.
 * 찾으면 본문이 시작하는 위치(out에서 몇 번째 바이트인지)를 돌려주고, 못 찾으면 -1을 돌려준다.
 * out은 '\0'으로 끝나 있어야 한다.
 */
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
            size_t body_len)
{
    /*
     * 1) 프로그램 파일이 있는지, 실행할 수 있는지 먼저 확인한다.
     * stat(경로, &st): 파일 정보를 채워 준다. 파일이 없으면 -1.
     * access(경로, X_OK): 이 프로세스가 그 파일을 실행할 권한이 있으면 0.
     */
    struct stat st;
    if (stat(program_path, &st) == -1) {
        return send_error(conn_fd, 404);
    }
    if (!S_ISREG(st.st_mode) || access(program_path, X_OK) == -1) {
        return send_error(conn_fd, 403);
    }

    /*
     * 2) 자식 프로세스에 넘길 환경 변수를 "이름=값" 문자열로 만든다.
     * execve의 셋째 인자는 이런 문자열들의 주소를 담은 배열이고, 배열의 끝은 NULL로 표시한다.
     */
    char env_method[64];
    char env_query[ENV_QUERY_SIZE];
    char env_length[64];
    snprintf(env_method, sizeof(env_method), "REQUEST_METHOD=%s", method);
    snprintf(env_query, sizeof(env_query), "QUERY_STRING=%s", query);
    snprintf(env_length, sizeof(env_length), "CONTENT_LENGTH=%zu", body_len);
    char *envp[] = {env_method, env_query, env_length, "GATEWAY_INTERFACE=CGI/1.1", "SERVER_PROTOCOL=HTTP/1.1", NULL};

    /* execve의 둘째 인자: 프로그램의 argv. argv[0]은 프로그램 이름이고 끝은 NULL */
    char *argv[] = {(char *)program_path, NULL};

    /*
     * 3) 파이프 두 개를 만든다.
     * pipe(fds): 커널 안에 한 방향 통로를 만들고 fd 두 개를 준다. fds[1]에 쓴 바이트를 fds[0]에서 읽는다.
     *   to_child   : 웹 서버 프로세스가 쓰고(to_child[1]) → CGI 프로세스가 표준 입력으로 읽는다(to_child[0])
     *   from_child : CGI 프로세스가 표준 출력으로 쓰고(from_child[1]) → 웹 서버 프로세스가 읽는다(from_child[0])
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

    /*
     * 4) fork: 지금 프로세스를 복사해 자식 프로세스를 만든다. 열려 있는 fd(파이프 포함)도 함께 복사된다.
     * fork는 두 프로세스에서 각각 돌아온다. 자식에서는 0, 부모에서는 자식의 프로세스 번호(pid)를 돌려준다.
     */
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return send_error(conn_fd, 500);
    }

    if (pid == 0) {
        /*
         * 여기는 자식 프로세스다.
         * dup2(a, b): fd b가 fd a와 같은 대상을 가리키게 만든다.
         *   표준 입력(0번)이 to_child의 읽는 쪽을, 표준 출력(1번)이 from_child의 쓰는 쪽을 가리키게 한다.
         *   그러면 CGI 프로그램은 평소처럼 stdin에서 읽고 printf로 쓰기만 해도 파이프로 주고받게 된다.
         */
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        /* 0번, 1번으로 복사했으므로 원래 번호 네 개와 연결 소켓은 자식에게 필요 없다 */
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        close(conn_fd);
        /* 웹 서버 프로세스는 SIGPIPE를 무시하게 해 두었다(main.c). CGI 프로그램은 기본 동작으로 되돌려 실행한다 */
        signal(SIGPIPE, SIG_DFL);

        /*
         * execve(경로, argv, envp): 이 프로세스의 프로그램을 경로의 프로그램으로 바꿔 실행한다.
         * 성공하면 돌아오지 않는다 (이 아래 코드는 실행되지 않는다). 프로세스 번호와 열린 fd(0, 1번)는 그대로다.
         * 돌아왔다면 실패한 것이다. 127로 끝내서 부모가 실패를 알 수 있게 한다.
         * _exit은 C 라이브러리의 정리 작업 없이 바로 끝낸다 (부모에게서 복사된 출력 버퍼를 두 번 내보내지 않게).
         */
        execve(program_path, argv, envp);
        perror("execve");
        _exit(127);
    }

    /*
     * 여기부터는 부모(웹 서버 프로세스)다.
     * 5) 부모가 쓰지 않는 쪽을 닫는다. 특히 from_child[1]을 닫지 않으면, 자식이 끝나도 쓰는 쪽이 하나 남아 있어서
     *    아래의 read가 끝(0)을 돌려받지 못하고 영원히 기다린다.
     */
    close(to_child[0]);
    close(from_child[1]);

    /* 6) POST 본문을 자식의 표준 입력으로 보내고, 쓰는 쪽을 닫는다. 닫아야 자식의 read가 끝(EOF)을 알 수 있다 */
    if (body_len > 0 && write_all(to_child[1], body, body_len) == -1) {
        perror("write (CGI 표준 입력)"); /* 자식이 본문을 읽지 않고 끝난 경우 등. 출력은 그대로 받아 본다 */
    }
    close(to_child[1]);

    /* 7) 자식의 표준 출력을 끝까지 읽는다. read가 0을 돌려주면 자식이 표준 출력을 닫은 것(보통 종료)이다 */
    char *out = malloc(CGI_OUT_MAX + 1);
    size_t out_len = 0;
    int too_large = 0;
    if (out == NULL) {
        perror("malloc");
    }
    while (out != NULL) {
        if (out_len == CGI_OUT_MAX) {
            too_large = 1; /* 버퍼가 가득 찼다. 더 받지 않는다 */
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

    /*
     * 8) waitpid(pid, &wstatus, 0): 자식이 끝날 때까지 기다리고, 어떻게 끝났는지 wstatus에 받는다.
     * 기다려 주지 않으면 끝난 자식의 기록이 커널에 계속 남는다 (좀비 프로세스).
     * WIFEXITED: 스스로 끝났으면(return 또는 exit) 참.  WEXITSTATUS: 그때의 종료 값. 관례로 0이 성공이다.
     */
    int wstatus = 0;
    while (waitpid(pid, &wstatus, 0) == -1) {
        if (errno == EINTR) {
            continue; /* 기다리는 도중 시그널이 왔을 뿐이다. 다시 기다린다 */
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

    /* 9) CGI 출력을 헤더와 본문으로 나눈다. 빈 줄이 없으면 형식이 틀린 출력이므로 500 */
    long body_offset = find_cgi_body(out);
    if (body_offset == -1) {
        free(out);
        return send_error(conn_fd, 500);
    }
    /* CGI가 쓴 헤더에서 Content-Type을 찾는다. 없으면 일반 글자로 본다 */
    char content_type[128];
    if (find_header(out, "Content-Type", content_type, sizeof(content_type)) != 0) {
        snprintf(content_type, sizeof(content_type), "text/plain; charset=utf-8");
    }

    /* 10) HTTP 응답으로 만들어 보낸다. Content-Length는 CGI 출력에서 본문 부분의 길이다 */
    size_t res_body_len = out_len - (size_t)body_offset;
    char head[256];
    int head_len = snprintf(head, sizeof(head),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            content_type, res_body_len);
    if (write_all(conn_fd, head, (size_t)head_len) == -1 ||
        write_all(conn_fd, out + body_offset, res_body_len) == -1) {
        perror("write");
    }
    free(out);
    return 200;
}
