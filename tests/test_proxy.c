/*
 * proxy.c, cache.c 단위 테스트: 설정 파일 읽기, 경로로 규칙 찾기, 라운드로빈, 절대 URL 나누기, 응답 캐시.
 * 각 테스트는 실패하면 FAIL 줄을 출력하고, 실패가 하나라도 있으면 종료 코드 1.
 */
/* mkstemp를 쓰려면 헤더보다 먼저 있어야 한다 (-std=c11) */
#define _POSIX_C_SOURCE 200809L

#include "proxy.h"

#include "cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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
/* 테스트용 시계: 캐시가 "지금"이라고 믿을 시각(밀리초)을 테스트가 정한다. 기다리지 않고 만료를 확인할 수 있다 */
static long long fake_time;

static long long fake_now(void)
{
    return fake_time;
}

/* key가 캐시에 있으면(hit) 1. 받은 복사본은 바로 해제한다 */
static int has(const char *key)
{
    char *got;
    size_t got_len;
    if (!cache_get(key, &got, &got_len)) {
        return 0;
    }
    free(got);
    return 1;
}

/*
 * 엣지 3 확인용: 스레드 여럿이 같은 키를 동시에 찾고, 저장하고, 시계를 만료 시각 너머로 넘긴다.
 * 잠금 구간이 잘못됐으면 해제한 메모리를 읽게 되고, make asan이 그것을 잡는다.
 */
static void *hammer(void *arg)
{
    long seed = (long)arg;
    for (int i = 0; i < 2000; i++) {
        char *got;
        size_t got_len;
        if (cache_get("/shared", &got, &got_len)) {
            if (got_len != 4 || memcmp(got, "body", 4) != 0) {
                failures++;
            }
            free(got);
        } else {
            cache_put("/shared", "body", 4);
        }
        if ((i + seed) % 7 == 0) {
            fake_time += CACHE_TTL_MS; /* 가끔 사본을 만료시킨다 */
        }
    }
    return NULL;
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

    /* 응답 캐시. 시계를 테스트용으로 바꾼다 */
    cache_clock = fake_now;
    fake_time = 0;
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
    }
    int hit0 = cache_get("/k0", &got, &got_len); /* /k0을 방금 썼으므로, 가장 오래 쓰지 않은 사본은 /k1이 된다 */
    if (hit0) {
        free(got);
    }
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

    /* ---- TTL 엣지 케이스 ---- */

    /* 1. 경계: 만료 시각 = 저장한 때 + TTL. 그 시각이 된 순간부터 만료다 */
    cache_clear();
    fake_time = 1000;
    cache_put("/a", "v1", 2);
    fake_time = 1000 + CACHE_TTL_MS - 1;
    CHECK(has("/a") == 1, "엣지 1: 만료 시각 1밀리초 전에는 hit");
    fake_time = 1000 + CACHE_TTL_MS;
    CHECK(has("/a") == 0, "엣지 1: 만료 시각이 된 순간은 miss (지금 >= 만료 시각)");

    /* 2. 지연 삭제: 만료된 사본은 찾은 그 자리에서 지워진다 */
    cache_clear();
    fake_time = 0;
    cache_put("/a", "12345", 5);
    cache_put("/b", "123", 3);
    fake_time = CACHE_TTL_MS;
    CHECK(cache_size() == 8, "엣지 2: 만료됐어도 아무도 찾지 않으면 메모리에 남아 있다");
    CHECK(has("/a") == 0 && cache_size() == 3, "엣지 2: 만료된 사본을 찾으면 miss이고, 그 사본은 지워진다");
    cache_put("/a", "new", 3);
    CHECK(has("/a") == 1 && cache_size() == 6, "엣지 2: miss 뒤에 넣은 새 사본으로 hit");

    /* 3. 확인과 교체 사이 끼어들기: A가 miss를 본 뒤 저장하기 전에 B가 같은 키를 저장한다 */
    cache_clear();
    fake_time = 0;
    cache_put("/a", "old", 3);
    fake_time = CACHE_TTL_MS;
    int a_missed = !has("/a");            /* 스레드 A: 만료를 보고 miss. 이제 백엔드에 다녀온다 */
    cache_put("/a", "from-B", 6);         /* 스레드 B: 그 사이에 같은 키를 저장한다 */
    cache_put("/a", "from-A", 6);         /* 스레드 A: 돌아와서 저장한다. 잠금 안에서 키로 다시 찾아 B의 사본을 바꾼다 */
    int ok3 = cache_get("/a", &got, &got_len) == 1 && got_len == 6 && memcmp(got, "from-A", 6) == 0;
    if (ok3) {
        free(got);
    }
    CHECK(a_missed && ok3 && cache_size() == 6, "엣지 3: 두 스레드가 같은 키를 차례로 저장해도 사본은 하나만 남는다");

    /* 3. 실제 스레드 넷으로: 동시에 찾고, 만료시키고, 저장한다 */
    cache_clear();
    int before = failures;
    pthread_t tids[4];
    for (long t = 0; t < 4; t++) {
        pthread_create(&tids[t], NULL, hammer, (void *)t);
    }
    for (int t = 0; t < 4; t++) {
        pthread_join(tids[t], NULL);
    }
    CHECK(failures == before && cache_size() <= 4, "엣지 3: 스레드 넷이 같은 키를 동시에 찾고 저장해도 사본은 하나 이하이고 내용이 깨지지 않는다");

    /* 4. hit은 만료 시각을 늘리지 않는다 */
    cache_clear();
    fake_time = 0;
    cache_put("/a", "v1", 2);
    fake_time = CACHE_TTL_MS - 1;
    CHECK(has("/a") == 1, "엣지 4: 만료 직전에 hit");
    fake_time = CACHE_TTL_MS;
    CHECK(has("/a") == 0, "엣지 4: 방금 hit했어도 만료 시각이 되면 miss (hit이 수명을 늘리지 않는다)");
    cache_put("/a", "v2", 2);
    fake_time = CACHE_TTL_MS + CACHE_TTL_MS - 1;
    CHECK(has("/a") == 1, "엣지 4: 다시 저장하면 그때부터 새로 TTL만큼 쓴다");

    /* 6. 자리가 모자라면 만료된 사본을 먼저 버린다. /old는 가장 최근에 쓴 사본이지만 만료됐다 */
    cache_clear();
    fake_time = 0;
    cache_put("/old", big, CACHE_OBJECT_MAX);
    fake_time = 5000;
    for (int i = 1; i < 10; i++) {
        snprintf(key, sizeof(key), "/k%d", i);
        cache_put(key, big, CACHE_OBJECT_MAX);
    }
    int old_hit = has("/old"); /* /old를 방금 썼다. LRU만 보면 가장 오래 쓰지 않은 사본은 /k1이다 */
    fake_time = CACHE_TTL_MS;  /* /old(0에 저장)는 만료, /k1~/k9(5000에 저장)는 아직 아니다 */
    cache_put("/new", big, CACHE_OBJECT_MAX);
    CHECK(old_hit == 1 && has("/old") == 0 && has("/k1") == 1 && has("/new") == 1,
          "엣지 6: 만료된 사본(/old)을 먼저 버리고, 가장 오래 쓰지 않은 사본(/k1)은 남긴다");

    free(big);
    cache_clear();

    if (failures > 0) {
        printf("단위 테스트 실패 %d개\n", failures);
        return 1;
    }
    printf("프록시 단위 테스트 모두 통과\n");
    return 0;
}
