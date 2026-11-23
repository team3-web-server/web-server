#include "netlib.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* ---------- 파일 ---------- */

void Close(int fd)
{
    if (close(fd) < 0) {
        unix_error("Close error");
    }
}

/* ---------- 소켓과 주소 변환 ---------- */

void Setsockopt(int fd, int level, int optname, const void *optval, int optlen)
{
    if (setsockopt(fd, level, optname, optval, optlen) < 0) {
        unix_error("Setsockopt error");
    }
}

void Getaddrinfo(const char *host, const char *service,
                 const struct addrinfo *hints, struct addrinfo **result)
{
    int rc = getaddrinfo(host, service, hints, result);
    if (rc != 0) {
        gai_error(rc, "Getaddrinfo error");
    }
}

void Freeaddrinfo(struct addrinfo *list)
{
    freeaddrinfo(list);
}

/* ---------- Rio ---------- */

/* n바이트를 읽을 때까지 반복한다. EOF를 만나면 실제로 읽은 바이트 수를 반환한다. */
ssize_t rio_readn(int fd, void *buf, size_t n)
{
    char *next = buf;
    size_t left = n;

    while (left > 0) {
        ssize_t got = read(fd, next, left);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (got == 0) {
            break; /* EOF */
        }
        next += got;
        left -= got;
    }
    return (ssize_t)(n - left);
}

ssize_t rio_writen(int fd, const void *buf, size_t n)
{
    const char *next = buf;
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

/* 연결 버퍼에 남은 바이트를 먼저 쓰고, 모자라면 fd에서 더 읽어 n바이트를 채운다. */
ssize_t rio_readnb(rio_t *rp, void *buf, size_t n)
{
    char *out = buf;
    size_t left = n;

    while (left > 0) {
        ssize_t got = rio_read(rp, out, left);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            break; /* EOF */
        }
        out += got;
        left -= got;
    }
    return (ssize_t)(n - left);
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

/* ---------- 연결 도우미 ---------- */

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

int Open_listenfd(char *port)
{
    int fd = open_listenfd(port);
    if (fd < 0) {
        unix_error("Open_listenfd error");
    }
    return fd;
}
