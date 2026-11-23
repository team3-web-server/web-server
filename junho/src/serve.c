/*
 * 정적 파일 서빙과 CGI 실행.
 * struct request를 파일 경로/CGI 인자로 해석하고 응답을 보낸다.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "serve.h"

#include "netlib.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define CGI_PREFIX "/cgi-bin/"
#define DEFAULT_PAGE "home.html"

extern char **environ;

static const char cgi_status_line[] = "HTTP/1.0 200 OK\r\nServer: Tiny Web Server\r\n";

enum uri_kind { URI_STATIC, URI_CGI, URI_FORBIDDEN };

/* '..' 경로 조각만 거부한다. 퍼센트 인코딩(%2e%2e)은 해석하지 않는다. */
static int has_parent_segment(const char *path)
{
    const char *p = path;

    while ((p = strstr(p, "..")) != NULL) {
        int left_ok = (p == path || p[-1] == '/');
        int right_ok = (p[2] == '\0' || p[2] == '/' || p[2] == '?');
        if (left_ok && right_ok) {
            return 1;
        }
        p += 2;
    }
    return 0;
}

/* uri를 ./ 기준 filename과 쿼리 문자열 cgiargs로 나눈다. uri의 '?'는 '\0'으로 바뀐다. */
static enum uri_kind parse_uri(char *uri, char *filename, size_t filename_size,
                               char *cgiargs, size_t cgiargs_size)
{
    char *query = strchr(uri, '?');
    if (query != NULL) {
        snprintf(cgiargs, cgiargs_size, "%s", query + 1);
        *query = '\0';
    } else {
        cgiargs[0] = '\0';
    }

    if (has_parent_segment(uri)) {
        return URI_FORBIDDEN;
    }

    snprintf(filename, filename_size, ".%s", uri);
    if (strncmp(uri, CGI_PREFIX, strlen(CGI_PREFIX)) == 0) {
        return URI_CGI;
    }

    size_t len = strlen(filename);
    if (len > 0 && filename[len - 1] == '/') {
        if (len + strlen(DEFAULT_PAGE) >= filename_size) {
            return URI_FORBIDDEN;
        }
        strcat(filename, DEFAULT_PAGE);
    }
    return URI_STATIC;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t s_len = strlen(s), suffix_len = strlen(suffix);
    return s_len >= suffix_len && strcmp(s + s_len - suffix_len, suffix) == 0;
}

static const char *get_filetype(const char *filename)
{
    static const struct {
        const char *ext;
        const char *type;
    } types[] = {
        {".html", "text/html"},
        {".css", "text/css"},
        {".js", "text/javascript"},
        {".gif", "image/gif"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".mpg", "video/mpeg"},
        {".mpeg", "video/mpeg"},
    };

    for (size_t i = 0; i < ARRAY_LEN(types); i++) {
        if (ends_with(filename, types[i].ext)) {
            return types[i].type;
        }
    }
    return "text/plain";
}

/* 정적 파일은 malloc + rio_readn + rio_writen으로 보낸다. */
static void serve_static(int fd, const char *filename, off_t filesize, int head_only)
{
    char header[MAXBUF];

    int srcfd = open(filename, O_RDONLY);
    if (srcfd < 0) {
        send_error(fd, 500, filename, "Tiny couldn't open the file", head_only);
        return;
    }

    char *body = NULL;
    if (filesize > 0) {
        body = malloc((size_t)filesize);
        if (body == NULL) {
            close(srcfd);
            send_error(fd, 500, filename, "Tiny couldn't allocate the file buffer", head_only);
            return;
        }
        if (rio_readn(srcfd, body, (size_t)filesize) != (ssize_t)filesize) {
            close(srcfd);
            free(body);
            send_error(fd, 500, filename, "Tiny couldn't read the file", head_only);
            return;
        }
    }
    close(srcfd);

    snprintf(header, sizeof(header),
             "HTTP/1.0 200 OK\r\n"
             "Server: Tiny Web Server\r\n"
             "Connection: close\r\n"
             "Content-length: %ld\r\n"
             "Content-type: %s\r\n\r\n",
             (long)filesize, get_filetype(filename));

    if (http_write_all(fd, header, strlen(header)) < 0) {
        free(body);
        return;
    }
    if (!head_only && body != NULL) {
        http_write_all(fd, body, (size_t)filesize);
    }
    free(body);
}

/* POST 본문을 tmpfile에 쓰고 처음으로 되감는다. 실패하면 NULL. */
static FILE *write_body_tmpfile(const struct request *req)
{
    FILE *tmp = tmpfile();
    if (tmp == NULL) {
        return NULL;
    }
    if (fwrite(req->body, 1, req->body_len, tmp) != req->body_len || fflush(tmp) != 0) {
        fclose(tmp);
        return NULL;
    }
    rewind(tmp);
    return tmp;
}

/* 자식 프로세스에서 표준 입출력을 연결하고 CGI 프로그램으로 바뀐다. 돌아오지 않는다. */
static void exec_cgi(const char *filename, const char *cgiargs,
                     const struct request *req, int stdin_fd, int stdout_fd)
{
    if (stdin_fd >= 0) {
        if (dup2(stdin_fd, STDIN_FILENO) < 0) {
            _exit(1);
        }
        if (stdin_fd != STDIN_FILENO) {
            close(stdin_fd);
        }
    }
    if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
        _exit(1);
    }
    if (stdout_fd != STDOUT_FILENO) {
        close(stdout_fd);
    }

    char content_length[32];
    snprintf(content_length, sizeof(content_length), "%ld",
             req->content_length != NO_CONTENT_LENGTH ? req->content_length : 0);
    setenv("REQUEST_METHOD", req->method_name, 1);
    setenv("QUERY_STRING", cgiargs, 1);
    setenv("CONTENT_LENGTH", content_length, 1);
    setenv("CONTENT_TYPE", req->content_type, 1);
    setenv("SCRIPT_NAME", req->uri, 1);
    setenv("SERVER_PROTOCOL", req->version, 1);

    char *argv[] = {(char *)filename, NULL};
    Execve(filename, argv, environ);
    _exit(1);
}

/* fd를 EOF까지 읽는다. max를 넘거나 메모리가 모자라면 그때까지 읽은 것만 돌려준다. */
static char *read_all(int fd, size_t max, size_t *len_out)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char chunk[4096];

    while (1) {
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (got == 0) {
            break;
        }
        size_t need = len + (size_t)got;
        if (need > max) {
            break;
        }
        if (need > cap) {
            size_t newcap = cap == 0 ? 8192 : cap * 2;
            while (newcap < need) {
                newcap *= 2;
            }
            char *grown = realloc(buf, newcap);
            if (grown == NULL) {
                break;
            }
            buf = grown;
            cap = newcap;
        }
        memcpy(buf + len, chunk, (size_t)got);
        len = need;
    }
    *len_out = len;
    return buf;
}

/* 빈 줄(\r\n\r\n)까지 포함한 헤더 길이. 빈 줄이 없으면 0. */
static size_t header_length(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
            return i + 4;
        }
    }
    return 0;
}

/* HEAD 요청: CGI 출력을 모두 받아 헤더만 클라이언트에 보낸다. */
static void send_cgi_head(int fd, const char *filename, int cgi_out)
{
    size_t len;
    char *output = read_all(cgi_out, MAX_BODY, &len);
    size_t header_len = output != NULL ? header_length(output, len) : 0;

    if (header_len == 0) {
        send_error(fd, 500, filename, "CGI did not produce HTTP headers", 1);
    } else {
        http_write_all(fd, cgi_status_line, strlen(cgi_status_line));
        http_write_all(fd, output, header_len);
    }
    free(output);
}

/* Wait를 부르지 않는다. 자식은 SIGCHLD 핸들러가 회수한다. */
static void serve_dynamic(int fd, const char *filename, const char *cgiargs,
                          const struct request *req, int head_only)
{
    FILE *body = NULL;
    if (req->method == HTTP_POST) {
        body = write_body_tmpfile(req);
        if (body == NULL) {
            send_error(fd, 500, filename, "Tiny couldn't store the request body", head_only);
            return;
        }
    }

    int pipefd[2] = {-1, -1};
    if (head_only && pipe(pipefd) < 0) {
        if (body != NULL) {
            fclose(body);
        }
        send_error(fd, 500, filename, "Tiny couldn't create a pipe", 1);
        return;
    }

    int body_fd = body != NULL ? fileno(body) : -1;
    int cgi_stdout = head_only ? pipefd[1] : fd;

    if (Fork() == 0) {
        if (head_only) {
            close(pipefd[0]);
        } else if (http_write_all(cgi_stdout, cgi_status_line,
                                  strlen(cgi_status_line)) < 0) {
            _exit(1); /* 클라이언트가 이미 끊겼으면 CGI를 실행하지 않는다 */
        }
        exec_cgi(filename, cgiargs, req, body_fd, cgi_stdout);
    }

    if (body != NULL) {
        fclose(body);
    }
    if (head_only) {
        close(pipefd[1]);
        send_cgi_head(fd, filename, pipefd[0]);
        close(pipefd[0]);
    }
}

int serve_request(int fd, struct request *req)
{
    char filename[MAXLINE], cgiargs[MAXLINE];
    int head_only = (req->method == HTTP_HEAD);

    enum uri_kind kind = parse_uri(req->uri, filename, sizeof(filename),
                                   cgiargs, sizeof(cgiargs));
    if (kind == URI_FORBIDDEN) {
        send_error(fd, 403, req->uri, "Tiny rejected a path outside the document root", head_only);
        return -1;
    }

    struct stat st;
    if (stat(filename, &st) < 0) {
        send_error(fd, 404, filename, "Tiny couldn't find this file", head_only);
        return -1;
    }

    if (kind == URI_STATIC) {
        if (!S_ISREG(st.st_mode) || (st.st_mode & S_IRUSR) == 0) {
            send_error(fd, 403, filename, "Tiny couldn't read the file", head_only);
            return -1;
        }
        if (req->method == HTTP_POST) {
            send_error(fd, 405, req->method_name, "POST is only supported for CGI", head_only);
            return -1;
        }
        serve_static(fd, filename, st.st_size, head_only);
    } else {
        if (!S_ISREG(st.st_mode) || (st.st_mode & S_IXUSR) == 0) {
            send_error(fd, 403, filename, "Tiny couldn't run the CGI program", head_only);
            return -1;
        }
        serve_dynamic(fd, filename, cgiargs, req, head_only);
    }
    return 0;
}
