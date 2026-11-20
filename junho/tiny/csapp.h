/*
 * csapp.h - CS:APP 3판 csapp.h 중 11장 예제(echo, hostinfo, Tiny, adder)가 쓰는 부분만 직접 옮겨 적은 헤더.
 * 원본 전체(시그널, 스레드, 세마포어 래퍼 등)는 넣지 않았다. 대문자로 시작하는 함수는
 * 원래 함수를 호출하고, 실패하면 에러 메시지를 출력한 뒤 프로세스를 끝내는 래퍼다.
 */
#ifndef __CSAPP_H__
#define __CSAPP_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* 기본 파일 권한: 사용자·그룹·기타 모두 읽기/쓰기 */
#define DEF_MODE   S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH
#define DEF_UMASK  S_IWGRP|S_IWOTH

/* connect, bind, accept에 넘길 때 쓰는 짧은 타입 이름 (Stevens 방식, 11.4.1) */
typedef struct sockaddr SA;

/* Rio(Robust I/O) 버퍼 (10.5절) */
#define RIO_BUFSIZE 8192
typedef struct {
    int rio_fd;                /* 이 버퍼가 읽는 디스크립터 */
    int rio_cnt;               /* 버퍼에 남아 있는 아직 안 읽은 바이트 수 */
    char *rio_bufptr;          /* 버퍼에서 다음에 읽을 위치 */
    char rio_buf[RIO_BUFSIZE]; /* 내부 버퍼 */
} rio_t;

/* execve에 넘길 환경 변수 목록 (libc가 정의) */
extern char **environ;

#define MAXLINE  8192  /* 텍스트 한 줄의 최대 길이 */
#define MAXBUF   8192  /* I/O 버퍼의 최대 크기 */
#define LISTENQ  1024  /* listen()의 두 번째 인자 */

/* 에러 출력 함수 */
void unix_error(char *msg);
void gai_error(int code, char *msg);
void app_error(char *msg);

/* 프로세스 제어 래퍼 */
pid_t Fork(void);
void Execve(const char *filename, char *const argv[], char *const envp[]);
pid_t Wait(int *status);

/* Unix I/O 래퍼 */
int Open(const char *pathname, int flags, mode_t mode);
void Close(int fd);
int Dup2(int fd1, int fd2);

/* 메모리 매핑 래퍼 */
void *Mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
void Munmap(void *start, size_t length);

/* 표준 I/O 래퍼 */
char *Fgets(char *ptr, int n, FILE *stream);
void Fputs(const char *ptr, FILE *stream);

/* 소켓 인터페이스 래퍼 */
void Setsockopt(int s, int level, int optname, const void *optval, int optlen);
int Accept(int s, struct sockaddr *addr, socklen_t *addrlen);

/* 프로토콜 독립 래퍼 */
void Getaddrinfo(const char *node, const char *service,
                 const struct addrinfo *hints, struct addrinfo **res);
void Getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                 size_t hostlen, char *serv, size_t servlen, int flags);
void Freeaddrinfo(struct addrinfo *res);

/* Rio 패키지 */
ssize_t rio_writen(int fd, void *usrbuf, size_t n);
void rio_readinitb(rio_t *rp, int fd);
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen);

/* Rio 래퍼 */
void Rio_writen(int fd, void *usrbuf, size_t n);
void Rio_readinitb(rio_t *rp, int fd);
ssize_t Rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen);

/* 클라이언트·서버 도우미 함수 (11.4.8) */
int open_clientfd(char *hostname, char *port);
int open_listenfd(char *port);

/* 도우미 함수 래퍼 */
int Open_clientfd(char *hostname, char *port);
int Open_listenfd(char *port);

#endif /* __CSAPP_H__ */
