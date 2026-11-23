#ifndef HANDLER_H
#define HANDLER_H

/*
 * 연결 하나의 요청을 읽고 응답을 보낸다. conn_fd는 닫지 않는다. root는 정적 파일 폴더 경로.
 * 경로가 /cgi-bin/으로 시작하면 CGI 프로그램을 실행하고, 아니면 파일을 보낸다.
 */
void handle_connection(int conn_fd, const char *root);

#endif /* HANDLER_H */
