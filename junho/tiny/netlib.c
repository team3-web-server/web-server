/*
 * netlib.c - netlib.h의 구현. CS:APP 3판 csapp.c에서 책 예제가 쓰는 함수만 다시 구현했다.
 * 동작은 원본과 같게 맞췄다: 소문자 함수는 실패 시 -1을 반환하고,
 * 대문자 래퍼는 실패하면 메시지를 출력하고 프로세스를 끝낸다.
 * 원본과 다른 점 하나: 종료 코드가 exit(0)이 아니라 exit(1)이다 (실패를 스크립트·테스트가 알 수 있도록).
 */
#include "netlib.h"

/**************************
 * 에러 출력 함수
 **************************/

/* Unix 스타일 에러: 실패 원인이 errno에 있음 */
void unix_error(char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/* getaddrinfo 스타일 에러: 실패 원인이 반환값(code)에 있음 */
void gai_error(int code, char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, gai_strerror(code));
    exit(1);
}

/* 애플리케이션 에러 */
void app_error(char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/*********************************************
 * 프로세스 제어 래퍼
 *********************************************/

pid_t Fork(void)
{
    pid_t pid;

    if ((pid = fork()) < 0)
        unix_error("Fork error");
    return pid;
}

void Execve(const char *filename, char *const argv[], char *const envp[])
{
    if (execve(filename, argv, envp) < 0)
        unix_error("Execve error");
}

pid_t Wait(int *status)
{
    pid_t pid;

    if ((pid = wait(status)) < 0)
        unix_error("Wait error");
    return pid;
}

/********************************
 * Unix I/O 래퍼
 ********************************/

int Open(const char *pathname, int flags, mode_t mode)
{
    int rc;

    if ((rc = open(pathname, flags, mode)) < 0)
        unix_error("Open error");
    return rc;
}

void Close(int fd)
{
    if (close(fd) < 0)
        unix_error("Close error");
}

int Dup2(int fd1, int fd2)
{
    int rc;

    if ((rc = dup2(fd1, fd2)) < 0)
        unix_error("Dup2 error");
    return rc;
}

/***************************************
 * 메모리 매핑 래퍼
 ***************************************/

void *Mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    void *ptr;

    if ((ptr = mmap(addr, len, prot, flags, fd, offset)) == MAP_FAILED)
        unix_error("mmap error");
    return ptr;
}

void Munmap(void *start, size_t length)
{
    if (munmap(start, length) < 0)
        unix_error("munmap error");
}

/******************************************
 * 표준 I/O 래퍼
 ******************************************/

/* fgets는 EOF와 에러 모두 NULL을 돌려주므로 ferror로 둘을 구분한다 */
char *Fgets(char *ptr, int n, FILE *stream)
{
    char *rptr;

    if (((rptr = fgets(ptr, n, stream)) == NULL) && ferror(stream))
        app_error("Fgets error");
    return rptr;
}

void Fputs(const char *ptr, FILE *stream)
{
    if (fputs(ptr, stream) == EOF)
        unix_error("Fputs error");
}

/****************************
 * 소켓 인터페이스 래퍼
 ****************************/

void Setsockopt(int s, int level, int optname, const void *optval, int optlen)
{
    if (setsockopt(s, level, optname, optval, optlen) < 0)
        unix_error("Setsockopt error");
}

int Accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    int rc;

    if ((rc = accept(s, addr, addrlen)) < 0)
        unix_error("Accept error");
    return rc;
}

/************************
 * 프로토콜 독립 래퍼
 ************************/

void Getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res)
{
    int rc;

    if ((rc = getaddrinfo(node, service, hints, res)) != 0)
        gai_error(rc, "Getaddrinfo error");
}

void Getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                 size_t hostlen, char *serv, size_t servlen, int flags)
{
    int rc;

    if ((rc = getnameinfo(sa, salen, host, hostlen, serv, servlen, flags)) != 0)
        gai_error(rc, "Getnameinfo error");
}

void Freeaddrinfo(struct addrinfo *res)
{
    freeaddrinfo(res);
}

/****************************************
 * Rio 패키지 (10.5절): 짧은 읽기·쓰기(short count)를 알아서 처리하는 I/O
 ****************************************/

/* n바이트를 다 쓸 때까지 write를 반복한다 (그림 10.4) */
ssize_t rio_writen(int fd, void *usrbuf, size_t n)
{
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;

    while (nleft > 0) {
        if ((nwritten = write(fd, bufp, nleft)) <= 0) {
            if (errno == EINTR)  /* 시그널 핸들러가 끼어들어 중단됨 */
                nwritten = 0;    /* 다시 write 호출 */
            else
                return -1;       /* write가 설정한 errno */
        }
        nleft -= nwritten;
        bufp += nwritten;
    }
    return n;
}

/*
 * rio_read - 내부 버퍼를 거치는 read (그림 10.7).
 * 버퍼가 비어 있으면 read로 채우고, 버퍼에서 최대 n바이트를 usrbuf로 복사한다.
 */
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n)
{
    int cnt;

    while (rp->rio_cnt <= 0) {  /* 버퍼가 비었으면 다시 채운다 */
        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0) {
            if (errno != EINTR)  /* 시그널 중단이 아니면 에러 */
                return -1;
        }
        else if (rp->rio_cnt == 0)  /* EOF */
            return 0;
        else
            rp->rio_bufptr = rp->rio_buf; /* 버퍼 포인터를 처음으로 */
    }

    /* min(n, rp->rio_cnt)바이트를 복사 */
    cnt = n;
    if (rp->rio_cnt < (int)n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

/* 디스크립터 fd와 읽기 버퍼를 연결한다 (그림 10.7) */
void rio_readinitb(rio_t *rp, int fd)
{
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

/* 텍스트 한 줄(줄바꿈 포함)을 최대 maxlen-1바이트 읽고 끝에 NUL을 붙인다 (그림 10.8) */
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen)
{
    int rc;
    size_t n;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++) {
        if ((rc = rio_read(rp, &c, 1)) == 1) {
            *bufp++ = c;
            if (c == '\n') {
                n++;
                break;
            }
        } else if (rc == 0) {
            if (n == 1)
                return 0; /* EOF, 읽은 데이터 없음 */
            else
                break;    /* EOF, 일부 데이터는 읽음 */
        } else
            return -1;    /* 에러 */
    }
    *bufp = 0;
    return n-1;
}

/**********************************
 * Rio 래퍼
 **********************************/

void Rio_writen(int fd, void *usrbuf, size_t n)
{
    if (rio_writen(fd, usrbuf, n) != (ssize_t)n)
        unix_error("Rio_writen error");
}

void Rio_readinitb(rio_t *rp, int fd)
{
    rio_readinitb(rp, fd);
}

ssize_t Rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen)
{
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0)
        unix_error("Rio_readlineb error");
    return rc;
}

/********************************
 * 클라이언트·서버 도우미 함수 (11.4.8, 그림 11.18·11.19)
 ********************************/

int open_clientfd(char *hostname, char *port) {
    int clientfd;
    struct addrinfo hints, *listp, *p;

    /* Get a list of potential server addresses */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM; /* Open a connection */
    hints.ai_flags = AI_NUMERICSERV; /* ... using a numeric port arg. */
    hints.ai_flags |= AI_ADDRCONFIG; /* Recommended for connections */
    Getaddrinfo(hostname, port, &hints, &listp);

    /* Walk the list for one that we can successfully connect to */
    for (p = listp; p; p = p->ai_next) {
        /* Create a socket descriptor */
        if ((clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue; /* Socket failed, try the next */

        /* Connect to the server */
        if (connect(clientfd, p->ai_addr, p->ai_addrlen) != -1)
            break; /* Success */
        Close(clientfd); /* Connect failed, try another */
    }

    /* Clean up */
    Freeaddrinfo(listp);
    if (!p) /* All connects failed */
        return -1;
    else    /* The last connect succeeded */
        return clientfd;
}

int open_listenfd(char *port)
{
    struct addrinfo hints, *listp, *p;
    int listenfd, optval=1;

    /* Get a list of potential server addresses */
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;             /* Accept connections */
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG; /* ... on any IP address */
    hints.ai_flags |= AI_NUMERICSERV;            /* ... using port number */
    Getaddrinfo(NULL, port, &hints, &listp);

    /* Walk the list for one that we can bind to */
    for (p = listp; p; p = p->ai_next) {
        /* Create a socket descriptor */
        if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue; /* Socket failed, try the next */

        /* Eliminates "Address already in use" error from bind */
        Setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   (const void *)&optval , sizeof(int));

        /* Bind the descriptor to the address */
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break; /* Success */
        Close(listenfd); /* Bind failed, try the next */
    }

    /* Clean up */
    Freeaddrinfo(listp);
    if (!p) /* No address worked */
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0) {
        Close(listenfd);
        return -1;
    }
    return listenfd;
}

/*******************************************
 * 도우미 함수 래퍼
 *******************************************/

int Open_clientfd(char *hostname, char *port)
{
    int rc;

    if ((rc = open_clientfd(hostname, port)) < 0)
        unix_error("Open_clientfd error");
    return rc;
}

int Open_listenfd(char *port)
{
    int rc;

    if ((rc = open_listenfd(port)) < 0)
        unix_error("Open_listenfd error");
    return rc;
}
