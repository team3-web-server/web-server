#include "response.h"

#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FILE_BUF_SIZE 8192 /* 파일을 한 번에 읽어 보내는 크기 */

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
    case 502: return "Bad Gateway";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

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
    /* 마지막 '.'부터가 확장자다. 그 '.'이 마지막 '/'보다 앞이면 폴더 이름의 점이므로 확장자가 아니다 */
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (dot == NULL || (slash != NULL && dot < slash)) {
        return "application/octet-stream";
    }

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
    /* Content-Length에 본문 길이를 적어야 하므로 본문을 먼저 만든다 */
    char body[128];
    int body_len = snprintf(body, sizeof(body), "<h1>%d %s</h1>\n", code, status_text(code));

    const char *allow = "";
    if (code == 405) {
        allow = "Allow: GET\r\n";
    }

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

int send_file(int fd, const char *path, int keep_alive)
{
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

    struct stat st;
    if (fstat(file_fd, &st) == -1) {
        perror("fstat");
        close(file_fd);
        return send_error(fd, 500);
    }
    /* 폴더는 보내지 않는다 */
    if (!S_ISREG(st.st_mode)) {
        close(file_fd);
        return send_error(fd, 403);
    }

    char head[256];
    int head_len = snprintf(head, sizeof(head),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %lld\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            mime_type(path), (long long)st.st_size, keep_alive ? "keep-alive" : "close");
    if (write_all(fd, head, (size_t)head_len) == -1) {
        perror("write");
        close(file_fd);
        return 200;
    }

    /* 파일을 버퍼 크기만큼씩 읽어 바로 보낸다. 길이는 read의 반환값을 쓴다 (파일 내용은 문자열이 아니다) */
    char buf[FILE_BUF_SIZE];
    for (;;) {
        ssize_t n = read(file_fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n == -1) {
            /* 헤더를 이미 보냈으므로 상태 코드를 바꿀 수 없다. 보내기를 멈춘다 */
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
