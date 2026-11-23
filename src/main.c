/*
 * 웹 서버 진입점.
 * 실행: ./build/server [포트] [정적 파일 폴더]     예) ./build/server 8080 ./www
 *       인자를 생략하면 포트 8080, 폴더 ./www를 쓴다.
 * 흐름: 명령행 인자를 읽는다 → TCP 소켓을 만들고 포트에 bind → listen (여기까지 한 번)
 *       → [연결 하나를 accept → fork → 자식 프로세스가 handle_connection으로 요청을 읽고 응답을 보낸 뒤 끝남]을 반복한다.
 * 연결마다 자식 프로세스를 하나 만들므로(process-per-connection) 여러 연결을 동시에 처리한다.
 * 끝내기: 터미널에서 Ctrl+C(SIGINT)를 누르거나 kill(SIGTERM)을 보내면, 새 연결을 받지 않고,
 *         처리 중인 자식 프로세스가 끝나기를 기다린 뒤 main이 return 0으로 끝난다. 한 번 더 누르면 기다리지 않고 바로 끝난다.
 */
/* sigaction, sigemptyset을 쓰려면 헤더보다 먼저 이 매크로가 있어야 한다 (-std=c11에서는 기본으로 꺼져 있다) */
#define _POSIX_C_SOURCE 200809L

#include "handler.h"
#include "net.h"

#include <errno.h>      /* errno */
#include <signal.h>     /* signal, sigaction, sigemptyset, SIGPIPE, SIGINT, SIGTERM, SIGCHLD, SIG_IGN, SIG_DFL */
#include <stdio.h>      /* printf, fprintf, perror, setvbuf */
#include <stdlib.h>     /* strtol, exit */
#include <sys/socket.h> /* SOMAXCONN */
#include <sys/types.h>  /* pid_t */
#include <sys/wait.h>   /* waitpid, WNOHANG */
#include <unistd.h>     /* close, fork, getpid, _exit */

#define DEFAULT_PORT 8080
#define DEFAULT_ROOT "./www" /* 정적 파일을 두는 폴더. 요청 대상을 이 폴더 아래의 파일 경로로 바꾼다 */
#define BACKLOG SOMAXCONN    /* 연결 대기열 최대 길이. SOMAXCONN은 <sys/socket.h>에 정의된 시스템 상한 값 */

/*
 * 종료 요청이 들어왔는지 적어 두는 변수. 처음에는 0이고, SIGINT나 SIGTERM이 오면 시그널 핸들러가 1로 바꾼다.
 * main의 반복문이 이 값을 보고 반복을 끝낸다.
 *   volatile     : 이 변수는 코드의 흐름과 상관없이(시그널 핸들러에서) 바뀔 수 있으니, 쓸 때마다 메모리에서 다시 읽으라는 표시
 *   sig_atomic_t : 시그널 핸들러 안에서 안전하게 읽고 쓸 수 있다고 C 표준이 정한 정수 타입
 */
static volatile sig_atomic_t stop_requested = 0;

/*
 * 시그널 핸들러: 시그널이 오면 커널이 하던 일을 잠시 멈추고 이 함수를 실행시킨다.
 * 핸들러는 프로그램이 무엇을 하던 중이었든 끼어들어 실행되므로, 안에서 할 수 있는 일이 매우 제한된다
 * (printf, malloc 같은 함수는 부르면 안 된다). 그래서 변수 하나만 바꾸고 바로 돌아간다.
 */
static void on_stop_signal(int signo)
{
    (void)signo; /* 어떤 시그널인지는 쓰지 않는다. 안 쓰는 인자라는 경고를 없애는 표기 */
    if (stop_requested) {
        /*
         * 종료 요청이 이미 한 번 있었는데 또 왔다 (Ctrl+C를 두 번 누른 경우).
         * 아무것도 보내지 않는 클라이언트를 기다리느라 반복문으로 돌아가지 못하는 상황일 수 있으므로 바로 끝낸다.
         * _exit은 시그널 핸들러 안에서 불러도 되는 함수다 (exit은 안 된다).
         */
        _exit(1);
    }
    stop_requested = 1;
}

/*
 * SIGCHLD 핸들러: 자식 프로세스가 끝나면 커널이 부모에게 SIGCHLD를 보낸다.
 * 끝난 자식의 종료 기록은 부모가 waitpid로 거둘 때까지 커널에 남는다 (좀비 프로세스).
 * waitpid(-1, NULL, WNOHANG): 끝난 자식이 있으면 하나 거두고 그 pid를 돌려준다.
 *   끝난 자식이 없으면 기다리지 않고 0을 돌려준다(WNOHANG). 자식이 아예 없으면 -1.
 * 시그널 한 번에 자식 여러 개가 끝나 있을 수 있으므로, 더 거둘 것이 없을 때까지 반복한다.
 * waitpid가 errno를 바꿀 수 있으므로, 끼어들기 전의 값을 저장했다가 되돌려 놓는다.
 */
static void reap_children(int signo)
{
    (void)signo;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

/*
 * 문자열 text를 포트 번호로 바꾼다. 1~65535 범위의 숫자가 아니면 -1을 돌려준다.
 * strtol(문자열, &end, 10): 문자열 앞부분의 10진수를 long으로 바꾸고, 숫자가 끝난 위치를 end에 넣어 준다.
 * 숫자가 하나도 없으면 end가 text와 같고, 숫자 뒤에 다른 글자가 남아 있으면 *end가 '\0'이 아니다.
 */
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

/*
 * main(argc, argv): 셸이 명령 줄을 공백으로 나눠 넘겨준다.
 *   argc는 칸의 개수, argv는 각 칸의 문자열이다. argv[0]은 프로그램 이름이다.
 *   예) ./build/server 8080 ./www → argc = 3, argv[1] = "8080", argv[2] = "./www"
 */
int main(int argc, char *argv[])
{
    /*
     * 표준 출력을 줄 단위로 내보내게 한다(_IOLBF: 줄바꿈 문자를 만나면 바로 내보냄).
     * 출력이 터미널이 아니라 파일이면 C 라이브러리는 출력을 모아 두었다가 한꺼번에 내보낸다.
     * 이 프로세스는 Ctrl+C로 끝나기 때문에, 모아 둔 출력이 나가지 못한 채 사라지지 않게 한다.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /*
     * SIGPIPE 시그널을 무시한다(SIG_IGN).
     * 읽는 쪽이 이미 닫힌 파이프나 소켓에 write하면 커널이 SIGPIPE를 보내는데, 기본 동작은 프로세스 종료다.
     * CGI 프로그램이 본문을 읽지 않고 먼저 끝나면, 웹 서버 프로세스가 그 파이프에 write하다가 함께 끝나 버린다.
     * 무시해 두면 write가 -1(errno는 EPIPE)을 돌려주고 프로세스는 계속 실행된다.
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * SIGINT(터미널의 Ctrl+C)와 SIGTERM(kill 명령의 기본 시그널)이 오면 on_stop_signal을 실행하도록 등록한다.
     * 두 시그널의 기본 동작은 "프로세스를 그 자리에서 종료"다. 그러면 듣기 소켓을 닫는 코드가 실행되지 못하고,
     * 처리 중이던 응답도 도중에 끊긴다. 핸들러를 등록하면 종료하는 대신 stop_requested만 1이 된다.
     * sigaction(시그널, 새 설정, 예전 설정을 받을 곳):
     *   sa_handler : 실행할 함수
     *   sa_mask    : 핸들러가 실행되는 동안 추가로 막을 시그널들. sigemptyset으로 비워 둔다
     *   sa_flags   : 0. SA_RESTART를 넣지 않았으므로, 시그널이 오면 기다리던 accept가 -1(errno는 EINTR)로 돌아온다.
     *                그래야 연결이 없어도 반복문이 stop_requested를 확인하러 갈 수 있다.
     */
    struct sigaction sa;
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    /*
     * SIGCHLD가 오면 reap_children을 실행하도록 등록한다.
     * SA_RESTART: 핸들러가 끝난 뒤, 중단됐던 accept를 커널이 자동으로 다시 시작한다.
     *             자식이 끝난 것은 반복문이 알 필요가 없는 일이라서 accept를 깨우지 않는다.
     */
    struct sigaction sa_child;
    sa_child.sa_handler = reap_children;
    sigemptyset(&sa_child.sa_mask);
    sa_child.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa_child, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    /* 명령행 인자: 첫째는 포트, 둘째는 정적 파일 폴더. 없으면 기본값을 쓴다 */
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

    /* 소켓을 만든다 */
    int fd = create_tcp_socket();
    if (fd == -1) {
        return 1;
    }
    printf("socket fd = %d\n", fd);

    /*
     * 서버를 끄고 바로 다시 켜도 bind가 되도록 주소 재사용 허용 옵션(SO_REUSEADDR)을 켠다. bind보다 먼저 해야 한다.
     * 웹 서버 프로세스가 연결을 먼저 닫으면 커널이 그 연결 기록을 약 60초 동안 TIME_WAIT 상태로 남겨 둔다.
     * 그동안 서버를 다시 실행하면 bind가 EADDRINUSE로 실패하는데, 이 옵션을 켜면 그 경우에도 bind가 된다.
     */
    if (set_reuseaddr(fd) == -1) {
        close(fd);
        return 1;
    }

    /* 소켓에 모든 IPv4 주소의 port번 포트를 붙인다 */
    if (bind_port(fd, port) == -1) {
        close(fd);
        return 1;
    }
    printf("bind 성공: 0.0.0.0:%d\n", port);

    /* 소켓을 듣기 소켓으로 바꾼다. 이때부터 커널이 이 포트로 오는 연결 요청을 받아 대기열에 넣는다 */
    if (listen_port(fd, BACKLOG) == -1) {
        close(fd);
        return 1;
    }
    printf("listen 중: 포트 %d, 정적 파일 폴더 %s (다른 터미널에서 curl localhost:%d, 끝내려면 Ctrl+C)\n", port, root,
           port);

    /*
     * 여기부터는 [연결 하나를 accept → fork → 자식 프로세스가 그 연결을 처리]를 반복한다.
     * 위의 socket, bind, listen은 프로세스가 시작할 때 한 번만 한다. 듣기 소켓 fd는 반복하는 동안 계속 열려 있다.
     * 부모 프로세스는 연결을 자식에게 맡기자마자 다음 accept로 돌아가므로, 여러 연결이 동시에 처리된다.
     * 종료 요청(SIGINT, SIGTERM)이 오면 stop_requested가 1이 되어 반복을 끝낸다.
     */
    while (!stop_requested) {
        /* 연결 하나를 받는다. 연결이 올 때까지 여기서 멈춰 있는다. conn_fd는 그 연결 전용의 새 fd */
        int conn_fd = accept_client(fd);
        if (conn_fd == -1) {
            /*
             * 연결을 받지 못했다. 시그널 때문에 accept가 중단된 경우도 여기로 온다.
             * continue로 반복문의 조건(!stop_requested)을 다시 확인한다. 종료 요청이었다면 반복이 끝난다.
             */
            continue;
        }

        /*
         * fork: 지금 프로세스를 복사해 자식 프로세스를 만든다. 열려 있는 fd(듣기 소켓, 연결 소켓)도 복사된다.
         * 자식에서는 0, 부모에서는 자식의 프로세스 번호(pid)를 돌려준다. 실패하면 -1.
         */
        pid_t pid = fork();
        if (pid == -1) {
            /* 프로세스를 더 만들 수 없다. 이 연결은 처리하지 못하고 닫는다 */
            perror("fork");
            close(conn_fd);
            continue;
        }

        if (pid == 0) {
            /*
             * 여기는 자식 프로세스다. 이 연결 하나만 처리하고 끝난다.
             * 자식은 새 연결을 받지 않으므로 듣기 소켓을 닫는다 (부모의 듣기 소켓은 그대로 열려 있다).
             * SIGCHLD는 기본 동작으로 되돌린다. CGI를 실행할 때 run_cgi가 waitpid로 CGI 프로세스의 종료 값을
             * 직접 받아야 하는데, 부모에게서 물려받은 핸들러(reap_children)가 먼저 거둬 가면 받을 수 없다.
             */
            close(fd);
            signal(SIGCHLD, SIG_DFL);

            /* 요청을 읽고 응답을 보낸다 (src/handler.c) */
            handle_connection(conn_fd, root);

            /* 연결 소켓을 닫는다. 부모도 자기 몫을 닫았으므로, 이때 커널이 연결을 끝낸다(FIN) */
            close(conn_fd);
            printf("연결을 닫았습니다 (자식 프로세스 %d)\n", (int)getpid());
            exit(0); /* 자식은 반복문으로 돌아가지 않고 여기서 끝난다 */
        }

        /*
         * 여기는 부모 프로세스다. 연결은 자식이 처리하므로 부모는 자기 몫의 conn_fd를 닫는다.
         * fork로 복사된 fd는 부모와 자식이 같은 연결을 함께 가리킨다. 커널은 그 연결을 가리키는 fd가 모두 닫혀야
         * 연결을 끝낸다. 부모가 닫지 않으면 자식이 끝나도 연결이 남고, 부모의 fd 번호도 연결마다 하나씩 늘어난다.
         */
        close(conn_fd);
        printf("자식 프로세스 %d에게 연결을 맡겼습니다 (conn fd = %d)\n", (int)pid, conn_fd);
    }

    /*
     * 반복이 끝났다. 듣기 소켓을 닫아 새 연결을 받지 않게 하고, 처리 중인 자식 프로세스가 모두 끝날 때까지 기다린다.
     * waitpid(-1, NULL, 0): 아무 자식이나 하나 끝날 때까지 기다린다. 자식이 하나도 없으면 -1(errno는 ECHILD)을 돌려준다.
     * 시그널로 중단된 경우(EINTR)는 다시 기다린다.
     */
    printf("종료 요청을 받았습니다. 듣기 소켓을 닫고, 처리 중인 연결이 끝나기를 기다립니다\n");
    close(fd);
    while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {
    }
    printf("웹 서버 프로세스를 끝냅니다\n");
    return 0;
}
