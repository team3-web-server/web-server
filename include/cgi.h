#ifndef CGI_H
#define CGI_H

#include <stddef.h>

/*
 * CGI: 요청마다 프로그램을 실행하고 그 출력을 응답으로 보내는 방식.
 * program_path의 프로그램을 자식 프로세스로 실행하고, 그 표준 출력을 200 응답으로 conn_fd에 보낸다.
 * 넘기는 것: 환경 변수 REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH와 표준 입력(POST 본문).
 * 프로그램은 표준 출력에 "헤더 줄, 빈 줄, 본문"을 써야 한다.
 * keep_alive가 1이면 200 응답에 "Connection: keep-alive"를, 0이면 "Connection: close"를 넣는다.
 * 반환값: 보낸 상태 코드. 파일이 없으면 404, 실행할 수 없으면 403, 실행이나 출력에 문제가 있으면 500.
 */
int run_cgi(int conn_fd, const char *program_path, const char *method, const char *query, const char *body,
            size_t body_len, int keep_alive);

#endif /* CGI_H */
