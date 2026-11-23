/*
 * adder용 CGI. 책의 adder는 "1&2" 형태만 받지만, HTML GET 폼은
 * "num1=1&num2=2"를 보내므로 두 형태를 모두 받는다.
 * '&'가 없는 쿼리에서 크래시하던 책 코드도 0으로 채워 처리한다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_QUERY 1024

static int parse_param(const char *query, const char *name, int *out)
{
    size_t name_len = strlen(name);
    const char *p = query;

    while (*p != '\0') {
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            *out = atoi(p + name_len + 1);
            return 1;
        }
        p = strchr(p, '&');
        if (p == NULL) {
            break;
        }
        p++;
    }
    return 0;
}

/* 책 형식 "1&2". '&'가 없으면 두 번째 값은 0으로 둔다. */
static void parse_positional(const char *query, int *a, int *b)
{
    char buf[MAX_QUERY];
    snprintf(buf, sizeof(buf), "%s", query);

    char *amp = strchr(buf, '&');
    if (amp != NULL) {
        *amp = '\0';
        *b = atoi(amp + 1);
    }
    *a = atoi(buf);
}

int main(void)
{
    int a = 0, b = 0;
    const char *query = getenv("QUERY_STRING");

    if (query != NULL && strchr(query, '=') != NULL) {
        parse_param(query, "num1", &a);
        parse_param(query, "num2", &b);
    } else if (query != NULL) {
        parse_positional(query, &a, &b);
    }

    char content[512];
    snprintf(content, sizeof(content),
             "Welcome to add.com: THE Internet addition portal.\r\n<p>"
             "The answer is: %d + %d = %d\r\n<p>"
             "Thanks for visiting!\r\n",
             a, b, a + b);

    printf("Connection: close\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);
    return 0;
}
