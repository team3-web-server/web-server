/*
 * 웹 서버가 쓰는 CS:APP csapp 함수 중 필요한 것만 구현한 라이브러리.
 * 함수 이름은 책과 같다. 대문자로 시작하는 래퍼는 실패하면 메시지를 출력하고 exit(1)로 끝낸다.
 */
#ifndef NETLIB_H
#define NETLIB_H

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MAXLINE 8192
#define MAXBUF 8192
#define LISTENQ 1024

typedef struct sockaddr SA;

/* 에러 출력 후 종료 */
void unix_error(char *msg);
void gai_error(int code, char *msg);

/* 프로세스 */
pid_t Fork(void);
void Execve(const char *filename, char *const argv[], char *const envp[]);

/* 파일 */
void Close(int fd);

/* 소켓과 주소 변환 */
void Setsockopt(int fd, int level, int optname, const void *optval, int optlen);
void Getaddrinfo(const char *host, const char *service,
                 const struct addrinfo *hints, struct addrinfo **result);
void Freeaddrinfo(struct addrinfo *list);

/* Rio(Robust I/O): 짧은 읽기·쓰기를 알아서 이어 처리하는 I/O */
#define RIO_BUFSIZE 8192
typedef struct {
    int fd;
    size_t unread;            /* buf에 남은, 아직 읽지 않은 바이트 수 */
    char *next;               /* buf에서 다음에 읽을 위치 */
    char buf[RIO_BUFSIZE];
} rio_t;

ssize_t rio_readn(int fd, void *buf, size_t n);
ssize_t rio_readnb(rio_t *rp, void *buf, size_t n);
ssize_t rio_writen(int fd, const void *buf, size_t n);
void rio_readinitb(rio_t *rp, int fd);
ssize_t rio_readlineb(rio_t *rp, void *buf, size_t maxlen);

/* 연결 도우미 (CS:APP 11.4.8) */
int open_listenfd(char *port);
int Open_listenfd(char *port);

#endif
