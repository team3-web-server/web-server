#ifndef RESPONSE_H
#define RESPONSE_H

/* HTTP 응답을 만들어 보내는 함수 모음. 에러 응답에는 항상 "Connection: close"를 넣는다 */

/* 상태 코드의 설명 문구. 예) 404 → "Not Found" */
const char *status_text(int code);

/* 파일 확장자에 맞는 Content-Type 값. 모르는 확장자는 "application/octet-stream" */
const char *mime_type(const char *path);

/* 에러 응답을 보내고 code를 그대로 돌려준다. 405에는 "Allow: GET" 헤더를 넣는다 */
int send_error(int fd, int code);

/* path의 파일을 200 응답으로 보낸다. 없으면 404, 권한이 없거나 폴더면 403, 그 밖의 실패는 500. 보낸 코드를 돌려준다 */
/* keep_alive가 1이면 200 응답에 "Connection: keep-alive"를, 0이면 "Connection: close"를 넣는다 */
int send_file(int fd, const char *path, int keep_alive);

#endif /* RESPONSE_H */
