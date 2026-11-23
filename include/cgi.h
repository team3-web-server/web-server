#ifndef CGI_H
#define CGI_H

/*
 * CGI(Common Gateway Interface): 웹 서버 프로세스가 요청마다 다른 프로그램을 실행하고,
 * 그 프로그램의 출력을 응답으로 돌려주는 방식 (단계 8).
 * 정적 파일은 디스크에 있는 내용을 그대로 보내지만, CGI는 요청이 올 때마다 프로그램이 응답을 새로 만든다.
 */

#include <stddef.h> /* size_t */

/*
 * program_path의 프로그램을 자식 프로세스로 실행하고, 그 출력을 200 응답으로 conn_fd에 보낸다.
 * 요청 정보를 넘기는 방법:
 *   환경 변수 REQUEST_METHOD = method (예: "GET")
 *   환경 변수 QUERY_STRING   = query  (요청 대상의 '?' 뒤. 없으면 빈 문자열)
 *   환경 변수 CONTENT_LENGTH = body_len
 *   표준 입력                = body (POST 본문. GET이면 body_len이 0이고 아무것도 넣지 않는다)
 * CGI 프로그램은 표준 출력에 "헤더 줄들, 빈 줄, 본문"을 써야 한다. 예) "Content-Type: text/plain\r\n\r\nhello\n"
 * 웹 서버 프로세스는 빈 줄 앞에서 Content-Type을 찾고, 빈 줄 뒤를 응답 본문으로 보낸다.
 * 반환값: 보낸 상태 코드.
 *   200 : CGI 출력을 응답으로 보냈다
 *   404 : 프로그램 파일이 없다
 *   403 : 일반 파일이 아니거나 실행 권한이 없다
 *   500 : pipe, fork가 실패했다 / 프로그램을 실행하지 못했다 / 프로그램이 0이 아닌 값으로 끝났다 /
 *         출력에 빈 줄이 없다 / 출력이 1MiB를 넘는다
 */
int run_cgi(int conn_fd, const char *program_path, const char *method, const char *query, const char *body,
            size_t body_len);

#endif /* CGI_H */
