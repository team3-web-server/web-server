#ifndef NET_H
#define NET_H

/*
 * IPv4 TCP 소켓을 하나 만든다.
 * 성공: 커널이 fd 테이블에서 비어 있는 가장 작은 번호를 골라 돌려준 fd (0 이상)
 * 실패: perror로 errno 내용을 stderr에 출력하고 -1을 반환
 * 반환받은 fd는 호출한 쪽이 close()로 닫아야 한다.
 */
int create_tcp_socket(void);

/*
 * 소켓 fd에 "모든 IPv4 주소(INADDR_ANY)의 port번 포트"를 붙인다(bind).
 * 성공: 0
 * 실패: perror로 errno 내용을 stderr에 출력하고 -1을 반환
 *       예) 이미 다른 소켓이 쓰는 포트 → EADDRINUSE
 *           1024 미만 포트를 일반 사용자가 요청 → EACCES
 * fd를 닫는 것은 호출한 쪽의 몫이다.
 */
int bind_port(int fd, int port);

#endif
