/*
 * proxy.c 단위 테스트: 설정 파일 읽기, 경로로 규칙 찾기, 라운드로빈.
 * 각 테스트는 실패하면 FAIL 줄을 출력하고, 실패가 하나라도 있으면 종료 코드 1.
 */
#include "proxy.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg)                                           \
    do {                                                           \
        if (cond) {                                                \
            printf("PASS: %s\n", msg);                             \
        } else {                                                   \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                            \
        }                                                          \
    } while (0)

/* 내용이 text인 임시 설정 파일을 만들고 load_config의 결과를 돌려준다 */
static int load_text(const char *text, struct proxy_config *cfg)
{
    char path[] = "/tmp/test_proxy_conf_XXXXXX";
    int fd = mkstemp(path);
    if (fd == -1 || write(fd, text, strlen(text)) == -1) {
        perror("mkstemp/write");
        return -2;
    }
    close(fd);
    int result = load_config(path, cfg);
    unlink(path);
    return result;
}

int main(void)
{
    struct proxy_config cfg;

    /* 설정 파일 읽기 */
    int r = load_text("# 주석\n\n/api/ 127.0.0.1:8001 127.0.0.1:8003\n/static/\t127.0.0.1:8002\n", &cfg);
    CHECK(r == 0, "load_config: 규칙 두 줄(주석, 빈 줄 포함)을 읽으면 0");
    CHECK(cfg.route_count == 2, "load_config: 규칙 2개");
    CHECK(strcmp(cfg.routes[0].prefix, "/api/") == 0, "load_config: 첫 규칙의 접두사 /api/");
    CHECK(cfg.routes[0].backend_count == 2, "load_config: 첫 규칙의 백엔드 2개");
    CHECK(strcmp(cfg.routes[0].backends[1].host, "127.0.0.1") == 0 && cfg.routes[0].backends[1].port == 8003,
          "load_config: 첫 규칙의 둘째 백엔드 127.0.0.1:8003");
    CHECK(cfg.routes[1].backend_count == 1 && cfg.routes[1].backends[0].port == 8002,
          "load_config: 둘째 규칙의 백엔드 127.0.0.1:8002");

    /* 경로로 규칙 찾기 */
    CHECK(match_route(&cfg, "/api/users") == &cfg.routes[0], "match_route: /api/users → 첫 규칙");
    CHECK(match_route(&cfg, "/static/a.css") == &cfg.routes[1], "match_route: /static/a.css → 둘째 규칙");
    CHECK(match_route(&cfg, "/") == NULL, "match_route: / → 맞는 규칙 없음");
    CHECK(match_route(&cfg, "/apiv2/x") == NULL, "match_route: /apiv2/x 는 /api/ 로 시작하지 않음");

    /* 라운드로빈 */
    struct route *api = &cfg.routes[0];
    CHECK(pick_backend(api)->port == 8001, "pick_backend: 첫 번째 요청 → 8001");
    CHECK(pick_backend(api)->port == 8003, "pick_backend: 두 번째 요청 → 8003");
    CHECK(pick_backend(api)->port == 8001, "pick_backend: 세 번째 요청 → 다시 8001");
    struct route *st = &cfg.routes[1];
    CHECK(pick_backend(st)->port == 8002 && pick_backend(st)->port == 8002, "pick_backend: 백엔드가 하나면 늘 그 백엔드");

    /* 틀린 설정 */
    CHECK(load_config("/nonexistent/proxy.conf", &cfg) == -1, "load_config: 없는 파일이면 -1");
    CHECK(load_text("api/ 127.0.0.1:8001\n", &cfg) == -1, "load_config: 접두사가 /로 시작하지 않으면 -1");
    CHECK(load_text("/api/\n", &cfg) == -1, "load_config: 백엔드가 없으면 -1");
    CHECK(load_text("/api/ 127.0.0.1\n", &cfg) == -1, "load_config: 포트가 없으면 -1");
    CHECK(load_text("/api/ 127.0.0.1:99999\n", &cfg) == -1, "load_config: 포트가 범위 밖이면 -1");

    if (failures > 0) {
        printf("단위 테스트 실패 %d개\n", failures);
        return 1;
    }
    printf("프록시 단위 테스트 모두 통과\n");
    return 0;
}
