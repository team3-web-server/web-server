/*
 * 책 예제(Tiny, adder, 에코, hostinfo)가 쓰는 CS:APP csapp 함수만 다시 구현한 라이브러리.
 * 함수 이름은 책과 같다. 대문자로 시작하는 래퍼는 실패하면 메시지를 출력하고 exit(1)로 끝낸다.
 */
#ifndef NETLIB_H
#define NETLIB_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXLINE 8192
#define MAXBUF 8192
#define LISTENQ 1024

typedef struct sockaddr SA;

extern char **environ;

/* 에러 출력 후 종료 */
void unix_error(char *msg);
void gai_error(int code, char *msg);
void app_error(char *msg);

/* 프로세스 */
pid_t Fork(void);
void Execve(const char *filename, char *const argv[], char *const envp[]);
pid_t Wait(int *status);

/* 파일과 메모리 매핑 */
int Open(const char *pathname, int flags, mode_t mode);
void Close(int fd);
int Dup2(int oldfd, int newfd);
void *Mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
void Munmap(void *addr, size_t len);

/* 표준 I/O */
char *Fgets(char *buf, int size, FILE *stream);
void Fputs(const char *str, FILE *stream);

/* 소켓과 주소 변환 */
void Setsockopt(int fd, int level, int optname, const void *optval, int optlen);
int Accept(int listenfd, struct sockaddr *addr, socklen_t *addrlen);
void Getaddrinfo(const char *host, const char *service,
                 const struct addrinfo *hints, struct addrinfo **result);
void Getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char *host,
                 size_t hostlen, char *service, size_t servlen, int flags);
void Freeaddrinfo(struct addrinfo *list);

/* Rio(Robust I/O): 짧은 읽기·쓰기를 알아서 이어 처리하는 I/O */
#define RIO_BUFSIZE 8192
typedef struct {
    int fd;
    size_t unread;            /* buf에 남은, 아직 읽지 않은 바이트 수 */
    char *next;               /* buf에서 다음에 읽을 위치 */
    char buf[RIO_BUFSIZE];
} rio_t;

ssize_t rio_writen(int fd, void *buf, size_t n);
void rio_readinitb(rio_t *rp, int fd);
ssize_t rio_readlineb(rio_t *rp, void *buf, size_t maxlen);

void Rio_writen(int fd, void *buf, size_t n);
void Rio_readinitb(rio_t *rp, int fd);
ssize_t Rio_readlineb(rio_t *rp, void *buf, size_t maxlen);

/* 연결 도우미 (CS:APP 11.4.8) */
int open_clientfd(char *hostname, char *port);
int open_listenfd(char *port);
int Open_clientfd(char *hostname, char *port);
int Open_listenfd(char *port);

#endif
