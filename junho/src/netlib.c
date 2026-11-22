#include "netlib.h"

void unix_error(char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

void gai_error(int code, char *msg)
{
    fprintf(stderr, "%s: %s\n", msg, gai_strerror(code));
    exit(1);
}

void app_error(char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/* ---------- 프로세스 ---------- */

pid_t Fork(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        unix_error("Fork error");
    }
    return pid;
}

void Execve(const char *filename, char *const argv[], char *const envp[])
{
    if (execve(filename, argv, envp) < 0) {
        unix_error("Execve error");
    }
}

pid_t Wait(int *status)
{
    pid_t pid = wait(status);
    if (pid < 0) {
        unix_error("Wait error");
    }
    return pid;
}

/* ---------- 파일과 메모리 매핑 ---------- */

int Open(const char *pathname, int flags, mode_t mode)
{
    int fd = open(pathname, flags, mode);
    if (fd < 0) {
        unix_error("Open error");
    }
    return fd;
}

void Close(int fd)
{
    if (close(fd) < 0) {
        unix_error("Close error");
    }
}

int Dup2(int oldfd, int newfd)
{
    int fd = dup2(oldfd, newfd);
    if (fd < 0) {
        unix_error("Dup2 error");
    }
    return fd;
}

void *Mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    void *mapped = mmap(addr, len, prot, flags, fd, offset);
    if (mapped == MAP_FAILED) {
        unix_error("mmap error");
    }
    return mapped;
}

void Munmap(void *addr, size_t len)
{
    if (munmap(addr, len) < 0) {
        unix_error("munmap error");
    }
}

/* ---------- 표준 I/O ---------- */

char *Fgets(char *buf, int size, FILE *stream)
{
    char *line = fgets(buf, size, stream);
    /* fgets는 EOF에서도 NULL을 돌려주므로 진짜 에러인지 ferror로 구분한다 */
    if (line == NULL && ferror(stream)) {
        app_error("Fgets error");
    }
    return line;
}

void Fputs(const char *str, FILE *stream)
{
    if (fputs(str, stream) == EOF) {
        unix_error("Fputs error");
    }
}

/* ---------- 소켓과 주소 변환 ---------- */

void Setsockopt(int fd, int level, int optname, const void *optval, int optlen)
{
    if (setsockopt(fd, level, optname, optval, optlen) < 0) {
        unix_error("Setsockopt error");
    }
}

int Accept(int listenfd, struct sockaddr *addr, socklen_t *addrlen)
{
    int connfd = accept(listenfd, addr, addrlen);
    if (connfd < 0) {
        unix_error("Accept error");
    }
    return connfd;
}

void Getaddrinfo(const char *host, const char *service,
                 const struct addrinfo *hints, struct addrinfo **result)
{
    int rc = getaddrinfo(host, service, hints, result);
    if (rc != 0) {
        gai_error(rc, "Getaddrinfo error");
    }
}

void Getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char *host,
                 size_t hostlen, char *service, size_t servlen, int flags)
{
    int rc = getnameinfo(addr, addrlen, host, hostlen, service, servlen, flags);
    if (rc != 0) {
        gai_error(rc, "Getnameinfo error");
    }
}

void Freeaddrinfo(struct addrinfo *list)
{
    freeaddrinfo(list);
}

/* ---------- Rio ---------- */

ssize_t rio_writen(int fd, void *buf, size_t n)
{
    char *next = buf;
    size_t left = n;

    while (left > 0) {
        ssize_t written = write(fd, next, left);
        if (written <= 0) {
            if (errno == EINTR) {
                continue; /* 시그널 핸들러가 끼어들었을 뿐이니 다시 쓴다 */
            }
            return -1;
        }
        next += written;
        left -= written;
    }
    return n;
}

/* 내부 버퍼에서 최대 n바이트를 꺼낸다. 버퍼가 비었으면 read로 한 번에 채운다. */
static ssize_t rio_read(rio_t *rp, char *out, size_t n)
{
    while (rp->unread == 0) {
        ssize_t got = read(rp->fd, rp->buf, sizeof(rp->buf));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (got == 0) {
            return 0; /* EOF */
        }
        rp->unread = got;
        rp->next = rp->buf;
    }

    size_t count = n < rp->unread ? n : rp->unread;
    memcpy(out, rp->next, count);
    rp->next += count;
    rp->unread -= count;
    return count;
}

void rio_readinitb(rio_t *rp, int fd)
{
    rp->fd = fd;
    rp->unread = 0;
    rp->next = rp->buf;
}

/*
 * 줄바꿈까지(포함) 최대 maxlen-1바이트를 읽고 '\0'을 붙인다. 읽은 바이트 수를 반환한다.
 * 아무것도 읽기 전에 EOF면 0을 반환하고 buf는 건드리지 않는다 (책과 같은 동작).
 */
ssize_t rio_readlineb(rio_t *rp, void *buf, size_t maxlen)
{
    char *out = buf;
    size_t len = 0;

    while (len + 1 < maxlen) {
        char c;
        ssize_t rc = rio_read(rp, &c, 1);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            if (len == 0) {
                return 0;
            }
            break;
        }
        out[len++] = c;
        if (c == '\n') {
            break;
        }
    }
    out[len] = '\0';
    return len;
}

void Rio_writen(int fd, void *buf, size_t n)
{
    if (rio_writen(fd, buf, n) != (ssize_t)n) {
        unix_error("Rio_writen error");
    }
}

void Rio_readinitb(rio_t *rp, int fd)
{
    rio_readinitb(rp, fd);
}

ssize_t Rio_readlineb(rio_t *rp, void *buf, size_t maxlen)
{
    ssize_t len = rio_readlineb(rp, buf, maxlen);
    if (len < 0) {
        unix_error("Rio_readlineb error");
    }
    return len;
}

/* ---------- 연결 도우미 ---------- */

/* hostname:port 서버에 연결된 fd를 반환한다. 주소 후보를 차례로 시도하고, 모두 실패하면 -1. */
int open_clientfd(char *hostname, char *port)
{
    struct addrinfo hints, *list, *p;
    int clientfd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | AI_ADDRCONFIG;
    Getaddrinfo(hostname, port, &hints, &list);

    for (p = list; p != NULL; p = p->ai_next) {
        clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (clientfd < 0) {
            continue;
        }
        if (connect(clientfd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        Close(clientfd);
    }

    Freeaddrinfo(list);
    return p == NULL ? -1 : clientfd;
}

/* port에서 연결을 기다리는 듣기 소켓 fd를 반환한다. 모든 주소에서 실패하면 -1. */
int open_listenfd(char *port)
{
    struct addrinfo hints, *list, *p;
    int listenfd = -1;
    int reuse = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV;
    Getaddrinfo(NULL, port, &hints, &list);

    for (p = list; p != NULL; p = p->ai_next) {
        listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listenfd < 0) {
            continue;
        }
        /* 서버를 재시작해도 TIME_WAIT 연결 때문에 bind가 실패하지 않게 한다 */
        Setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        Close(listenfd);
    }

    Freeaddrinfo(list);
    if (p == NULL) {
        return -1;
    }
    if (listen(listenfd, LISTENQ) < 0) {
        Close(listenfd);
        return -1;
    }
    return listenfd;
}

int Open_clientfd(char *hostname, char *port)
{
    int fd = open_clientfd(hostname, port);
    if (fd < 0) {
        unix_error("Open_clientfd error");
    }
    return fd;
}

int Open_listenfd(char *port)
{
    int fd = open_listenfd(port);
    if (fd < 0) {
        unix_error("Open_listenfd error");
    }
    return fd;
}
