#ifndef NET_H
#define NET_H

#include <stddef.h> /* size_t */

/*
 * IPv4 TCP 소켓을 하나 만든다.
 * 성공: 커널이 fd 테이블에서 비어 있는 가장 작은 번호를 골라 돌려준 fd (0 이상)
 * 실패: perror로 errno 내용을 stderr에 출력하고 -1을 반환
 * 반환받은 fd는 호출한 쪽이 close()로 닫아야 한다.
 */
int create_tcp_socket(void);

/*
 * 소켓에 SO_REUSEADDR 옵션을 켠다. bind_port()보다 먼저 불러야 한다.
 * 서버가 연결을 먼저 닫으면 그 연결이 약 60초 동안 TIME_WAIT 상태로 남고,
 * 그동안 서버를 다시 실행하면 bind가 EADDRINUSE로 실패한다. 이 옵션을 켜면 그 경우에도 bind가 된다.
 * (같은 포트에 이미 듣고 있는 소켓이 있으면 이 옵션을 켜도 EADDRINUSE로 실패한다.)
 * 성공: 0
 * 실패: perror로 errno 내용을 stderr에 출력하고 -1을 반환
 */
int set_reuseaddr(int fd);

/*
 * 소켓 fd에 "모든 IPv4 주소(INADDR_ANY)의 port번 포트"를 붙인다(bind).
 * 성공: 0
 * 실패: perror로 errno 내용을 stderr에 출력하고 -1을 반환
 *       예) 이미 다른 소켓이 쓰는 포트 → EADDRINUSE
 *           1024 미만 포트를 일반 사용자가 요청 → EACCES
 * fd를 닫는 것은 호출한 쪽의 몫이다.
 */
int bind_port(int fd, int port);

/*
 * bind된 소켓을 "듣기 소켓"으로 바꾼다(listen).
 * 이때부터 커널이 이 포트로 오는 연결 요청(SYN)을 받아 3-way handshake를 하고,
 * 완료된 연결을 대기열에 넣는다. backlog는 그 대기열의 최대 길이.
 * 성공: 0
 * 실패: perror로 errno 내용을 stderr에 출력하고 -1을 반환
 */
int listen_port(int fd, int backlog);

/*
 * 듣기 소켓 fd의 대기열에서 완료된 연결 하나를 꺼낸다(accept).
 * 대기열이 비어 있으면 연결이 들어올 때까지 멈춰서 기다린다(블로킹).
 * 성공: 그 연결 전용의 새 fd(연결 소켓)를 반환하고, 클라이언트의 IP:포트를 stdout에 출력
 * 실패: perror로 errno 내용을 stderr에 출력하고 -1을 반환
 * 듣기 소켓 fd는 그대로 남는다. 반환받은 새 fd는 호출한 쪽이 close()로 닫아야 한다.
 */
int accept_client(int fd);

/*
 * buf의 len바이트를 fd에 "전부" 쓴다 (단계 10).
 * 블로킹 소켓의 write는 보통 요청한 바이트를 모두 쓴 뒤 돌아오지만, 쓰는 도중 시그널 핸들러가 실행되면
 * 일부만 쓰고 그 바이트 수를 돌려주거나, 하나도 못 쓰고 -1(errno는 EINTR)을 돌려줄 수 있다.
 * 이 서버는 SIGINT, SIGTERM에 핸들러를 등록했으므로(main.c) 그런 일이 실제로 생길 수 있다.
 * 그래서 남은 바이트가 0이 될 때까지 write를 반복한다.
 * 반환값: 모두 썼으면 0. write가 EINTR이 아닌 이유로 실패하면 -1 (errno는 그대로 남는다. 출력은 호출한 쪽이 한다).
 */
int write_all(int fd, const char *buf, size_t len);

#endif
