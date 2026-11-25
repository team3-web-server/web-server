/*
 * 웹 서버 진입점.
 * 실행: ./build/server [포트] [정적 파일 폴더]   (기본값: 8080, ./www)
 * 흐름: socket → bind → listen 뒤, [accept → fork → 자식 프로세스가 handle_connection]을 반복한다.
 * 종료: SIGINT(Ctrl+C)나 SIGTERM을 받으면 새 연결을 받지 않고, 처리 중인 자식 프로세스가 끝나기를 기다린 뒤 끝난다.
 */
/* sigaction을 쓰려면 헤더보다 먼저 있어야 한다 (-std=c11) */
#define _POSIX_C_SOURCE 200809L

#include "handler.h"
#include "net.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define DEFAULT_ROOT "./www"
#define BACKLOG SOMAXCONN

/* 종료 요청 여부. 시그널 핸들러가 1로 바꾸고 main의 반복문이 읽는다 */
static volatile sig_atomic_t stop_requested = 0;

/* SIGINT, SIGTERM 핸들러. 두 번째 시그널이면 기다리지 않고 바로 끝낸다 */
static void on_stop_signal(int signo)
{
    (void)signo;
    if (stop_requested) {
        _exit(1);
    }
    stop_requested = 1;
}

/* SIGCHLD 핸들러. 끝난 자식 프로세스를 모두 거둬 좀비 프로세스가 남지 않게 한다 */
static void reap_children(int signo)
{
    (void)signo;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

/* 문자열을 포트 번호로 바꾼다. 1~65535의 숫자가 아니면 -1 */
static int parse_port(const char *text)
{
    char *end;
    errno = 0;
    long port = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || port < 1 || port > 65535) {
        return -1;
    }
    return (int)port;
}

int main(int argc, char *argv[])
{
    /* 출력을 파일로 돌려도 줄마다 바로 나가게 한다 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 닫힌 파이프나 소켓에 write해도 프로세스가 끝나지 않고 write가 -1(EPIPE)을 돌려주게 한다 */
    signal(SIGPIPE, SIG_IGN);

    /* SA_RESTART를 넣지 않는다. 시그널이 오면 accept가 EINTR로 돌아와야 반복문이 stop_requested를 확인한다 */
    struct sigaction sa;
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    /* SA_RESTART: 자식 프로세스가 끝난 것 때문에 accept가 중단되지 않게 한다 */
#ifndef NO_FORK
    struct sigaction sa_child;
    sa_child.sa_handler = reap_children;
    sigemptyset(&sa_child.sa_mask);
    sa_child.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa_child, NULL) == -1) {
        perror("sigaction");
        return 1;
    }
#else
    /* fork 없는 빌드에서는 run_cgi의 waitpid가 CGI 프로세스를 직접 거두므로 SIGCHLD 핸들러를 달지 않는다 */
    (void)reap_children;
#endif

    int port = DEFAULT_PORT;
    const char *root = DEFAULT_ROOT;
    if (argc > 3) {
        fprintf(stderr, "사용법: %s [포트] [정적 파일 폴더]\n", argv[0]);
        return 1;
    }
    if (argc >= 2) {
        port = parse_port(argv[1]);
        if (port == -1) {
            fprintf(stderr, "포트는 1~65535 사이의 숫자여야 합니다: %s\n", argv[1]);
            return 1;
        }
    }
    if (argc >= 3) {
        root = argv[2];
    }

    int fd = create_tcp_socket();
    if (fd == -1) {
        return 1;
    }
    printf("socket fd = %d\n", fd);

    /* 재실행 직후 TIME_WAIT 때문에 bind가 실패하지 않게 한다. bind보다 먼저 부른다 */
    if (set_reuseaddr(fd) == -1) {
        close(fd);
        return 1;
    }

    if (bind_port(fd, port) == -1) {
        close(fd);
        return 1;
    }
    printf("bind 성공: 0.0.0.0:%d\n", port);

    if (listen_port(fd, BACKLOG) == -1) {
        close(fd);
        return 1;
    }
    printf("listen 중: 포트 %d, 정적 파일 폴더 %s (다른 터미널에서 curl localhost:%d, 끝내려면 Ctrl+C)\n", port, root,
           port);

    while (!stop_requested) {
        int conn_fd = accept_client(fd);
        if (conn_fd == -1) {
            /* 시그널로 중단된 경우도 여기로 온다. 반복 조건을 다시 확인한다 */
            continue;
        }

#ifdef NO_FORK
        /*
         * 비교용 빌드(-DNO_FORK, build/server-nofork): fork하지 않고 웹 서버 프로세스가 직접 연결을 처리한다.
         * 이 연결의 처리가 끝날 때까지 accept로 돌아가지 못하므로, 다음 클라이언트는 그동안 기다린다.
         */
        handle_connection(conn_fd, root);
        close(conn_fd);
        printf("연결을 닫았습니다 (fork 없음: 웹 서버 프로세스 %d가 직접 처리)\n", (int)getpid());
        continue;
#endif

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            close(conn_fd);
            continue;
        }

        /* 자식 프로세스: 이 연결 하나만 처리하고 끝난다. 듣기 소켓은 쓰지 않으므로 닫는다 */
        if (pid == 0) {
            close(fd);
            /* 기본 동작으로 되돌려야 run_cgi의 waitpid가 CGI 프로세스의 종료 값을 받을 수 있다 */
            signal(SIGCHLD, SIG_DFL);

            handle_connection(conn_fd, root);

            close(conn_fd);
            printf("연결을 닫았습니다 (자식 프로세스 %d)\n", (int)getpid());
            exit(0);
        }

        /* 부모 프로세스: 자기 몫의 conn_fd를 닫는다. 닫지 않으면 자식 프로세스가 끝나도 연결이 끝나지 않는다 */
        close(conn_fd);
        printf("자식 프로세스 %d에게 연결을 맡겼습니다 (conn fd = %d)\n", (int)pid, conn_fd);
    }

    /* 듣기 소켓을 닫고, 처리 중인 자식 프로세스가 모두 끝날 때까지 기다린다 */
    printf("종료 요청을 받았습니다. 듣기 소켓을 닫고, 처리 중인 연결이 끝나기를 기다립니다\n");
    close(fd);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {
    }
    printf("웹 서버 프로세스를 끝냅니다\n");
    return 0;
}
