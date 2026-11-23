/*
 * HTTP/1.0 웹 서버의 진입점.
 * listen/accept, SIGCHLD·SIGPIPE 처리, 연결 하나당 요청 하나를 처리한다.
 *
 * 사용법: build/server <port>   (www/에서 실행)
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "http.h"
#include "serve.h"

#include "netlib.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static void sigchld_handler(int sig)
{
    int saved = errno;
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        /* 종료한 CGI 자식을 모두 회수한다 */
    }
    errno = saved;
}

static int doit(int fd)
{
    struct request req;

    int rc = read_request(fd, &req);
    if (rc < 0) {
        return -1; /* 빈 연결 또는 EOF */
    }
    if (rc > 0) {
        send_error(fd, rc, req.uri[0] != '\0' ? req.uri : "-",
                   "Tiny couldn't parse the request", req.method == HTTP_HEAD);
        return -1;
    }

    int status = serve_request(fd, &req);
    free(req.body);
    return status;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        unix_error("sigaction");
    }

    int listenfd = Open_listenfd(argv[1]);
    while (1) {
        struct sockaddr_storage clientaddr;
        socklen_t clientlen = sizeof(clientaddr);
        int connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (connfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            unix_error("Accept error");
        }

        char host[NI_MAXHOST], port[NI_MAXSERV];
        if (getnameinfo((SA *)&clientaddr, clientlen,
                        host, sizeof(host), port, sizeof(port),
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            printf("Accepted connection from (%s, %s)\n", host, port);
        }

        doit(connfd);
        Close(connfd);
    }
}
