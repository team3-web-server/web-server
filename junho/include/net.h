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
 * 포트 문자열을 검사해 1~65535 사이의 정수로 바꾼다.
 * 성공: 포트 번호
 * 실패(NULL, 빈 문자열, 숫자가 아닌 글자, 범위 밖): -1
 */
int parse_port(const char *s);

/*
 * CS:APP 11.4.8 open_listenfd: port번 포트에서 연결 요청을 받을 듣기 소켓을 연다.
 * getaddrinfo가 돌려준 주소 목록을 차례로 socket → bind 해 보고, 성공한 fd를 listen 상태로 만든다.
 * 주소는 와일드카드(모든 IP)이고, port가 "0"이면 커널이 빈 포트를 고른다(테스트용).
 * 성공: 듣기 소켓 fd
 * 실패: 원인을 stderr에 출력하고 -1을 반환
 * 반환받은 fd는 호출한 쪽이 close()로 닫아야 한다.
 */
int open_listenfd(const char *port);

#endif
