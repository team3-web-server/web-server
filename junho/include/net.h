#ifndef NET_H
#define NET_H

/* IPv4 TCP 소켓 fd를 반환한다. 실패하면 원인을 출력하고 -1. */
int create_tcp_socket(void);

/* "1"~"65535"이면 포트 번호, 아니면 -1 (공백·부호·숫자 뒤 글자도 거부). */
int parse_port(const char *s);

/* port의 모든 IP 주소에서 연결을 기다리는 듣기 소켓 fd를 반환한다. 실패하면 원인을 출력하고 -1.
 * port가 "0"이면 커널이 빈 포트를 고른다. */
int open_listenfd(const char *port);

#endif
