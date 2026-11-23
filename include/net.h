#ifndef NET_H
#define NET_H

#include <stddef.h>

/* 아래 함수들은 모두 실패하면 perror로 원인을 출력하고 -1을 돌려준다 (write_all은 출력하지 않는다) */

/* IPv4 TCP 소켓을 만들어 fd를 돌려준다 */
int create_tcp_socket(void);

/* SO_REUSEADDR을 켠다. TIME_WAIT 연결이 남아 있어도 bind가 되게 한다. bind_port보다 먼저 부른다 */
int set_reuseaddr(int fd);

/* 소켓에 0.0.0.0:port를 붙인다 */
int bind_port(int fd, int port);

/* 소켓을 듣기 소켓으로 바꾼다. backlog는 연결 대기열의 최대 길이 */
int listen_port(int fd, int backlog);

/* 대기열에서 연결 하나를 꺼내 새 fd(연결 소켓)를 돌려준다. 연결이 없으면 올 때까지 기다린다 */
int accept_client(int fd);

/* buf의 len바이트를 전부 쓸 때까지 write를 반복한다. 모두 썼으면 0 */
int write_all(int fd, const char *buf, size_t len);

#endif
