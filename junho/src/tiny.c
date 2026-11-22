/*
 * Tiny: GET 요청에 정적 파일 또는 CGI 프로그램 출력으로 응답하는 반복 HTTP/1.0 서버 (CS:APP 11.6)
 * 사용법: ../build/tiny <port>   (junho/tiny에서 실행. 정적 파일은 현재 디렉터리, CGI 프로그램은 ./cgi-bin에서 찾는다)
 */
#include "netlib.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int listenfd = Open_listenfd(argv[1]);
    while (1) {
        struct sockaddr_storage clientaddr;
        socklen_t clientlen = sizeof(clientaddr);
        char hostname[MAXLINE], port[MAXLINE];

        int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        doit(connfd);
        Close(connfd);
    }
}

static int is_readable_file(struct stat *st)
{
    return S_ISREG(st->st_mode) && (st->st_mode & S_IRUSR);
}

static int is_executable_file(struct stat *st)
{
    return S_ISREG(st->st_mode) && (st->st_mode & S_IXUSR);
}

/* HTTP 요청 하나를 읽고 응답한다. */
void doit(int fd)
{
    char line[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    struct stat st;
    rio_t rio;

    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, line, MAXLINE);
    printf("Request headers:\n%s", line);
    sscanf(line, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET") != 0) {
        clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
        return;
    }
    read_requesthdrs(&rio);

    int is_static = parse_uri(uri, filename, cgiargs);
    if (stat(filename, &st) < 0) {
        clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
        return;
    }

    if (is_static) {
        if (!is_readable_file(&st)) {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
            return;
        }
        serve_static(fd, filename, st.st_size);
    } else {
        if (!is_executable_file(&st)) {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
            return;
        }
        serve_dynamic(fd, filename, cgiargs);
    }
}

/* 요청 헤더를 빈 줄("\r\n")까지 읽어 출력만 하고 버린다. */
void read_requesthdrs(rio_t *rp)
{
    char line[MAXLINE] = ""; /* 첫 읽기가 EOF여도 쓰레기 값을 출력하지 않도록 */

    do {
        Rio_readlineb(rp, line, MAXLINE);
        printf("%s", line);
    } while (strcmp(line, "\r\n") != 0);
}

/*
 * URI를 파일 경로와 CGI 인자로 나눈다. 정적 콘텐츠면 1, 동적(CGI)이면 0을 반환한다.
 * 예) "/"                   → "./home.html"
 *     "/cgi-bin/adder?1&2"  → "./cgi-bin/adder", 인자 "1&2"
 */
int parse_uri(char *uri, char *filename, char *cgiargs)
{
    if (strstr(uri, "cgi-bin") == NULL) {
        strcpy(cgiargs, "");
        sprintf(filename, ".%s", uri);
        if (uri[strlen(uri) - 1] == '/') {
            strcat(filename, "home.html");
        }
        return 1;
    }

    char *query = strchr(uri, '?');
    if (query != NULL) {
        strcpy(cgiargs, query + 1);
        *query = '\0';
    } else {
        strcpy(cgiargs, "");
    }
    sprintf(filename, ".%s", uri);
    return 0;
}

void serve_static(int fd, char *filename, int filesize)
{
    char filetype[32], header[MAXBUF]; /* get_filetype이 쓰는 값은 가장 길어도 "text/plain" */

    get_filetype(filename, filetype);
    snprintf(header, sizeof(header),
             "HTTP/1.0 200 OK\r\n"
             "Server: Tiny Web Server\r\n"
             "Connection: close\r\n"
             "Content-length: %d\r\n"
             "Content-type: %s\r\n\r\n",
             filesize, filetype);
    Rio_writen(fd, header, strlen(header));
    printf("Response headers:\n%s", header);

    /* 파일을 메모리에 매핑하면 read 루프 없이 한 번에 보낼 수 있다. 매핑 뒤에는 파일 fd가 필요 없다. */
    int srcfd = Open(filename, O_RDONLY, 0);
    char *body = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    Close(srcfd);
    Rio_writen(fd, body, filesize);
    Munmap(body, filesize);
}

void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html")) {
        strcpy(filetype, "text/html");
    } else if (strstr(filename, ".gif")) {
        strcpy(filetype, "image/gif");
    } else if (strstr(filename, ".png")) {
        strcpy(filetype, "image/png");
    } else if (strstr(filename, ".jpg")) {
        strcpy(filetype, "image/jpeg");
    } else {
        strcpy(filetype, "text/plain");
    }
}

/* 응답 줄과 Server 헤더만 보내고, 나머지 헤더와 본문은 CGI 프로그램이 표준 출력으로 쓴다. */
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
    char header[MAXLINE];
    char *no_args[] = {NULL};

    snprintf(header, sizeof(header), "HTTP/1.0 200 OK\r\nServer: Tiny Web Server\r\n");
    Rio_writen(fd, header, strlen(header));

    if (Fork() == 0) {
        setenv("QUERY_STRING", cgiargs, 1);
        Dup2(fd, STDOUT_FILENO);
        Execve(filename, no_args, environ);
    }
    Wait(NULL);
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
    char body[MAXBUF], header[MAXLINE];

    snprintf(body, sizeof(body),
             "<html><title>Tiny Error</title>"
             "<body bgcolor=ffffff>\r\n"
             "%s: %s\r\n"
             "<p>%s: %s\r\n"
             "<hr><em>The Tiny Web server</em>\r\n",
             errnum, shortmsg, longmsg, cause);
    snprintf(header, sizeof(header),
             "HTTP/1.0 %s %s\r\n"
             "Content-type: text/html\r\n"
             "Content-length: %d\r\n\r\n",
             errnum, shortmsg, (int)strlen(body));
    Rio_writen(fd, header, strlen(header));
    Rio_writen(fd, body, strlen(body));
}
