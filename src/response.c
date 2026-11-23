#include "response.h"

#include "net.h"

#include <errno.h>     /* errno, ENOENT, ENOTDIR, EACCES */
#include <fcntl.h>     /* open, O_RDONLY */
#include <stdio.h>     /* snprintf, perror */
#include <string.h>    /* strrchr, strchr */
#include <strings.h>   /* strcasecmp */
#include <sys/stat.h>  /* fstat, struct stat, S_ISREG */
#include <sys/types.h> /* ssize_t */
#include <unistd.h>    /* read, close */

#define FILE_BUF_SIZE 8192 /* 파일 내용을 한 번에 읽어 보내는 버퍼 크기(바이트) */

const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* 확장자와 MIME 타입의 짝을 적어 둔 표. 글자로 된 파일에는 글자 인코딩(charset=utf-8)도 알려 준다 */
static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".txt", "text/plain; charset=utf-8"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
};

const char *mime_type(const char *path)
{
    /*
     * strrchr(문자열, 글자): 글자가 "마지막으로" 나오는 위치. 없으면 NULL.
     * 마지막 '.'부터 끝까지가 확장자다. 단, 그 '.'이 마지막 '/'보다 앞에 있으면 폴더 이름의 점이므로 확장자가 아니다.
     * 예) "./www/index.html" → ".html",  "./www/v1.2/readme" → 확장자 없음
     */
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (dot == NULL || (slash != NULL && dot < slash)) {
        return "application/octet-stream";
    }

    /* 표를 처음부터 끝까지 보며 확장자가 같은 줄을 찾는다. strcasecmp는 대소문자를 구분하지 않고 비교한다 (같으면 0) */
    size_t count = sizeof(mime_table) / sizeof(mime_table[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(dot, mime_table[i].ext) == 0) {
            return mime_table[i].type;
        }
    }
    return "application/octet-stream";
}

int send_error(int fd, int code)
{
    /* 본문을 먼저 만든다. 헤더의 Content-Length에 본문 길이를 적어야 하기 때문이다 */
    char body[128];
    int body_len = snprintf(body, sizeof(body), "<h1>%d %s</h1>\n", code, status_text(code));

    /* 405 응답에는 이 경로가 허용하는 메서드를 Allow 헤더로 알려 준다 (RFC 9110 15.5.6절) */
    const char *allow = "";
    if (code == 405) {
        allow = "Allow: GET\r\n";
    }

    /* 상태 줄 + 헤더 + 빈 줄 + 본문을 한 버퍼에 만들어 write_all로 보낸다 */
    char res[512];
    int res_len = snprintf(res, sizeof(res),
                           "HTTP/1.1 %d %s\r\n"
                           "Content-Type: text/html; charset=utf-8\r\n"
                           "Content-Length: %d\r\n"
                           "%s"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           code, status_text(code), body_len, allow, body);
    if (write_all(fd, res, (size_t)res_len) == -1) {
        perror("write");
    }
    return code;
}

int send_file(int fd, const char *path)
{
    /*
     * 1) 파일을 읽기 전용으로 연다. 성공하면 그 파일을 가리키는 새 fd를 받는다.
     * 실패하면 errno에 이유가 들어 있다.
     *   ENOENT  : 그 이름의 파일이 없다           → 404
     *   ENOTDIR : 경로 중간이 폴더가 아니다       → 404
     *   EACCES  : 읽을 권한이 없다                → 403
     *   그 밖   : 서버 쪽 문제로 본다             → 500
     */
    int file_fd = open(path, O_RDONLY);
    if (file_fd == -1) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return send_error(fd, 404);
        }
        if (errno == EACCES) {
            return send_error(fd, 403);
        }
        perror("open");
        return send_error(fd, 500);
    }

    /*
     * 2) fstat(fd, &st): 열린 파일의 정보(크기, 종류 등)를 st에 채워 준다.
     * st.st_size가 파일 크기(바이트)이고, S_ISREG(st.st_mode)는 일반 파일이면 참이다.
     * 폴더도 open은 되지만 내용을 read할 수 없으므로 403으로 거절한다 (폴더 목록을 보여 주지 않는다).
     */
    struct stat st;
    if (fstat(file_fd, &st) == -1) {
        perror("fstat");
        close(file_fd);
        return send_error(fd, 500);
    }
    if (!S_ISREG(st.st_mode)) {
        close(file_fd);
        return send_error(fd, 403);
    }

    /* 3) 상태 줄과 헤더를 보낸다. Content-Length는 파일 크기, Content-Type은 확장자로 정한다 */
    char head[256];
    int head_len = snprintf(head, sizeof(head),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %lld\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            mime_type(path), (long long)st.st_size);
    if (write_all(fd, head, (size_t)head_len) == -1) {
        perror("write");
        close(file_fd);
        return 200;
    }

    /*
     * 4) 파일 내용을 보낸다. 파일 전체를 메모리에 올리지 않고 8192바이트씩 읽어서 바로 보낸다.
     * 파일 내용은 문자열이 아니라 바이트다 (이미지에는 값이 0인 바이트도 있다).
     * 그래서 strlen을 쓰지 않고 read가 돌려준 바이트 수 n만큼만 write_all로 쓴다.
     * read가 0을 돌려주면 파일 끝이다.
     */
    char buf[FILE_BUF_SIZE];
    for (;;) {
        ssize_t n = read(file_fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n == -1) {
            /* 헤더를 이미 보냈으므로 상태 코드를 바꿀 수 없다. 보내기를 멈추고 연결을 닫게 둔다 */
            perror("read");
            break;
        }
        if (write_all(fd, buf, (size_t)n) == -1) {
            perror("write");
            break;
        }
    }
    close(file_fd);
    return 200;
}
