#ifndef RESPONSE_H
#define RESPONSE_H

/*
 * HTTP 응답을 만들어 연결 소켓으로 보내는 함수 모음 (단계 4, 5).
 * 응답 메시지 = 상태 줄 + 헤더 줄들 + 빈 줄 + 본문.
 * 이 서버는 응답 하나를 보낸 뒤 연결을 닫으므로 모든 응답에 "Connection: close"를 넣는다.
 */

/*
 * 상태 코드의 설명 문구를 돌려준다. 예) 200 → "OK", 404 → "Not Found"
 * 이 서버가 쓰지 않는 코드는 "Unknown"을 돌려준다.
 */
const char *status_text(int code);

/*
 * 파일 경로의 확장자를 보고 Content-Type 헤더에 넣을 값(MIME 타입)을 돌려준다.
 * 예) "./www/index.html" → "text/html; charset=utf-8", "./www/img/red.png" → "image/png"
 * 확장자가 없거나 표에 없으면 "application/octet-stream"(종류를 모르는 바이트)을 돌려준다.
 * 대소문자는 구분하지 않는다 (".PNG"도 image/png).
 */
const char *mime_type(const char *path);

/*
 * 에러 응답을 보낸다. 본문은 "<h1>404 Not Found</h1>" 같은 짧은 HTML이다.
 * code가 405이면 허용하는 메서드를 알려 주는 "Allow: GET" 헤더를 함께 넣는다.
 * 반환값: 보낸 상태 코드(code 그대로). write가 실패해도 code를 돌려준다 (실패 내용은 stderr에 출력).
 */
int send_error(int fd, int code);

/*
 * path의 파일을 열어 200 응답으로 보낸다.
 *   1) open으로 파일을 연다. 없으면 404, 읽을 권한이 없으면 403, 그 밖의 실패는 500을 보낸다.
 *   2) fstat으로 파일 크기를 알아낸다. 일반 파일이 아니면(폴더 등) 403을 보낸다.
 *   3) 상태 줄과 헤더(Content-Type, Content-Length, Connection)를 보낸다.
 *   4) 파일 내용을 버퍼 크기만큼씩 read해서 그대로 write하는 일을 파일 끝까지 반복한다.
 * 반환값: 보낸 상태 코드 (200, 403, 404, 500 중 하나).
 */
int send_file(int fd, const char *path);

#endif /* RESPONSE_H */
