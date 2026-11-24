#ifndef HANDLER_H
#define HANDLER_H

/*
 * 연결 하나에서 요청을 읽고 응답을 보내기를 반복한다 (keep-alive). conn_fd는 닫지 않는다. root는 정적 파일 폴더 경로.
 * 반복을 멈추는 때: 요청이 연결을 닫자고 했을 때(Connection: close, 또는 keep-alive 없는 HTTP/1.0),
 * 클라이언트가 연결을 닫았을 때, 5초 동안 다음 요청이 없을 때, 에러 응답을 보냈을 때.
 * 경로가 /cgi-bin/으로 시작하면 CGI 프로그램을 실행하고, 아니면 파일을 보낸다.
 */
void handle_connection(int conn_fd, const char *root);

#endif /* HANDLER_H */
