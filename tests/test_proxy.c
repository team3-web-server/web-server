/*
 * proxy.c, cache.c 단위 테스트: 설정 파일 읽기, 경로로 규칙 찾기, 라운드로빈, 절대 URL 나누기, 응답 캐시.
 * 각 테스트는 실패하면 FAIL 줄을 출력하고, 실패가 하나라도 있으면 종료 코드 1.
 */
/* nanosleep, mkstemp를 쓰려면 헤더보다 먼저 있어야 한다 (-std=c11) */
#define _POSIX_C_SOURCE 200809L

#include "proxy.h"

#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
/* 2밀리초 쉰다. 캐시가 사본마다 "마지막으로 쓴 시각"(밀리초)을 다르게 기록하게 한다 */
static void short_sleep(void)
{
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, NULL);
}

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

    /* 자동 제외: 죽은 백엔드(alive가 0)는 건너뛴다 */
    CHECK(api->backends[0].alive == 1 && api->backends[1].alive == 1, "load_config: 백엔드는 처음에 살아 있다고 본다");
    api->backends[1].alive = 0;
    CHECK(pick_backend(api)->port == 8001 && pick_backend(api)->port == 8001 && pick_backend(api)->port == 8001,
          "pick_backend: 8003이 죽었으면 8001만 고른다");
    api->backends[0].alive = 0;
    CHECK(pick_backend(api) == NULL, "pick_backend: 모두 죽었으면 NULL");
    api->backends[1].alive = 1;
    CHECK(pick_backend(api)->port == 8003, "pick_backend: 8003이 살아나면 다시 고른다");
    api->backends[0].alive = 1;

    /* 틀린 설정 */
    CHECK(load_config("/nonexistent/proxy.conf", &cfg) == -1, "load_config: 없는 파일이면 -1");
    CHECK(load_text("api/ 127.0.0.1:8001\n", &cfg) == -1, "load_config: 접두사가 /로 시작하지 않으면 -1");
    CHECK(load_text("/api/\n", &cfg) == -1, "load_config: 백엔드가 없으면 -1");
    CHECK(load_text("/api/ 127.0.0.1\n", &cfg) == -1, "load_config: 포트가 없으면 -1");
    CHECK(load_text("/api/ 127.0.0.1:99999\n", &cfg) == -1, "load_config: 포트가 범위 밖이면 -1");

    /* 절대 URL 나누기 (Forward Proxy) */
    struct backend dest;
    const char *path;
    CHECK(parse_absolute_url("http://example.com/a/b?x=1", &dest, &path) == 0 && strcmp(dest.host, "example.com") == 0 &&
              dest.port == 80 && strcmp(path, "/a/b?x=1") == 0,
          "parse_absolute_url: http://example.com/a/b?x=1 → example.com, 80, /a/b?x=1");
    CHECK(parse_absolute_url("http://127.0.0.1:8001/api/", &dest, &path) == 0 && strcmp(dest.host, "127.0.0.1") == 0 &&
              dest.port == 8001 && strcmp(path, "/api/") == 0,
          "parse_absolute_url: 포트가 있으면 그 포트");
    CHECK(parse_absolute_url("http://example.com", &dest, &path) == 0 && strcmp(path, "/") == 0,
          "parse_absolute_url: 경로가 없으면 /");
    CHECK(parse_absolute_url("http://example.com:8080", &dest, &path) == 0 && dest.port == 8080 && strcmp(path, "/") == 0,
          "parse_absolute_url: 포트만 있고 경로가 없으면 /");
    CHECK(parse_absolute_url("/api/users", &dest, &path) == -1, "parse_absolute_url: http://로 시작하지 않으면 -1");
    CHECK(parse_absolute_url("https://example.com/", &dest, &path) == -1, "parse_absolute_url: https://는 -1");
    CHECK(parse_absolute_url("http:///a", &dest, &path) == -1, "parse_absolute_url: 주소가 비었으면 -1");
    CHECK(parse_absolute_url("http://example.com:abc/", &dest, &path) == -1, "parse_absolute_url: 포트가 숫자가 아니면 -1");
    CHECK(parse_absolute_url("http://example.com:99999/", &dest, &path) == -1, "parse_absolute_url: 포트가 범위 밖이면 -1");

    /* 설정 파일의 "cache 접두사" 줄 */
    CHECK(load_text("/static/ 127.0.0.1:8002\ncache /static/\n", &cfg) == 0 && cfg.cache_count == 1 && cfg.route_count == 1,
          "load_config: cache 줄은 규칙으로 세지 않고 캐시 접두사로 읽는다");
    CHECK(is_cache_path(&cfg, "/static/a.css?v=1") == 1, "is_cache_path: /static/a.css?v=1 → 캐시 대상");
    CHECK(is_cache_path(&cfg, "/api/users") == 0, "is_cache_path: /api/users → 캐시 대상 아님");
    CHECK(is_cache_path(&cfg, "http://127.0.0.1:8002/static/a.css") == 0, "is_cache_path: 절대 URL(Forward 요청) → 캐시 대상 아님");
    CHECK(load_text("cache static/\n", &cfg) == -1, "load_config: cache 뒤의 접두사가 /로 시작하지 않으면 -1");

    /* 응답 캐시 */
    char *got;
    size_t got_len;
    cache_clear();
    CHECK(cache_get("/a", &got, &got_len) == 0, "cache_get: 넣은 적 없는 키는 miss");
    cache_put("/a", "hello", 5);
    CHECK(cache_get("/a", &got, &got_len) == 1 && got_len == 5 && memcmp(got, "hello", 5) == 0, "cache_get: 넣은 사본을 그대로 돌려준다 (hit)");
    free(got);
    CHECK(cache_get("/a?x=1", &got, &got_len) == 0, "cache_get: 쿼리가 다르면 다른 키");
    cache_put("/a", "bye", 3);
    CHECK(cache_get("/a", &got, &got_len) == 1 && got_len == 3 && memcmp(got, "bye", 3) == 0, "cache_put: 같은 키로 다시 넣으면 사본이 바뀐다");
    free(got);

    /* 크기 제한: 사본 하나 100 KiB, 전체 1 MiB */
    char *big = malloc(CACHE_OBJECT_MAX + 1);
    memset(big, 'x', CACHE_OBJECT_MAX + 1);
    cache_put("/big", big, CACHE_OBJECT_MAX + 1);
    CHECK(cache_get("/big", &got, &got_len) == 0, "cache_put: 100 KiB보다 큰 사본은 저장하지 않는다");

    /* 100 KiB짜리 사본은 10개까지 들어간다(1,024,000바이트). 11번째를 넣으면 가장 오래 쓰지 않은 사본이 버려진다 */
    cache_clear();
    char key[16];
    for (int i = 0; i < 10; i++) {
        snprintf(key, sizeof(key), "/k%d", i);
        cache_put(key, big, CACHE_OBJECT_MAX);
        short_sleep();
    }
    int hit0 = cache_get("/k0", &got, &got_len); /* /k0을 방금 썼으므로, 가장 오래 쓰지 않은 사본은 /k1이 된다 */
    if (hit0) {
        free(got);
    }
    short_sleep();
    cache_put("/k10", big, CACHE_OBJECT_MAX);
    int has0 = cache_get("/k0", &got, &got_len);
    if (has0) {
        free(got);
    }
    int has1 = cache_get("/k1", &got, &got_len);
    if (has1) {
        free(got);
    }
    int has10 = cache_get("/k10", &got, &got_len);
    if (has10) {
        free(got);
    }
    CHECK(hit0 == 1 && has0 == 1 && has1 == 0 && has10 == 1, "cache_put: 전체 1 MiB를 넘으면 가장 오래 쓰지 않은 사본(/k1)을 버린다 (LRU)");
    free(big);
    cache_clear();

    if (failures > 0) {
        printf("단위 테스트 실패 %d개\n", failures);
        return 1;
    }
    printf("프록시 단위 테스트 모두 통과\n");
    return 0;
}
