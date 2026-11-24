/* clock_gettime을 쓰려면 헤더보다 먼저 있어야 한다 (-std=c11) */
#define _POSIX_C_SOURCE 200809L

#include "cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 사본 하나. data가 NULL이면 빈 칸 */
struct cache_entry {
    char key[CACHE_KEY_SIZE];
    char *data;
    size_t len;
    long long expire_at; /* 만료 시각(밀리초) = 저장한 때 + CACHE_TTL_MS. hit해도 바뀌지 않는다 */
    long long last_used; /* 마지막으로 쓴 순번: 저장한 때와 hit한 때 커진다. 작을수록 오래 쓰지 않은 사본 */
};

static struct cache_entry entries[CACHE_SLOTS];
static size_t total_size;   /* 사본 크기의 합 */
static long long use_count; /* 사본을 쓸 때마다 1씩 커지는 순번 */

/* entries, total_size, use_count를 읽고 쓰는 동안 잡는 잠금 */
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

/* 지금 시각(밀리초). 흐른 시간을 재는 시계(CLOCK_MONOTONIC)라서 컴퓨터의 날짜와 시각을 바꿔도 거꾸로 가지 않는다 */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long long (*cache_clock)(void) = now_ms;

/* 아래 함수들은 cache_lock을 잡은 채로 부른다 */

static struct cache_entry *find_entry(const char *key)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (entries[i].data != NULL && strcmp(entries[i].key, key) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

static struct cache_entry *find_empty(void)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (entries[i].data == NULL) {
            return &entries[i];
        }
    }
    return NULL;
}

/* 버릴 사본을 고른다: 만료된 사본이 있으면 그것, 없으면 가장 오래 쓰지 않은 사본. 사본이 하나도 없으면 NULL */
static struct cache_entry *pick_victim(long long now)
{
    struct cache_entry *oldest = NULL;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        struct cache_entry *entry = &entries[i];
        if (entry->data == NULL) {
            continue;
        }
        if (now >= entry->expire_at) {
            return entry;
        }
        if (oldest == NULL || entry->last_used < oldest->last_used) {
            oldest = entry;
        }
    }
    return oldest;
}

static void remove_entry(struct cache_entry *entry)
{
    free(entry->data);
    entry->data = NULL;
    total_size -= entry->len;
}

int cache_get(const char *key, char **data, size_t *len)
{
    int hit = 0;
    pthread_mutex_lock(&cache_lock);
    long long now = cache_clock();
    struct cache_entry *entry = find_entry(key);
    if (entry != NULL && now >= entry->expire_at) {
        /*
         * 만료됐다: 그 자리에서 지우고 miss로 처리한다 (지연 삭제).
         * 만료 확인과 삭제를 같은 잠금 구간에서 하므로, 그 사이에 다른 스레드가 넣은 새 사본을 지우는 일이 없다.
         */
        remove_entry(entry);
        entry = NULL;
    }
    if (entry != NULL) {
        /* 잠금을 푼 뒤에는 이 사본이 버려질 수 있으므로, 잠금 안에서 복사해 둔다 */
        char *copy = malloc(entry->len + 1);
        if (copy != NULL) {
            memcpy(copy, entry->data, entry->len);
            copy[entry->len] = '\0';
            *data = copy;
            *len = entry->len;
            entry->last_used = ++use_count; /* 만료 시각(expire_at)은 건드리지 않는다 */
            hit = 1;
        }
    }
    pthread_mutex_unlock(&cache_lock);
    return hit;
}

int cache_put(const char *key, const char *data, size_t len, long long fetched_at)
{
    if (len > CACHE_OBJECT_MAX || strlen(key) >= CACHE_KEY_SIZE) {
        return 0;
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return 0;
    }
    /* 만료 시각은 저장하는 지금이 아니라 백엔드에 물어보기 시작한 때부터 센다. 응답을 받는 데 걸린 시간만큼 내용은 이미 낡았다 */
    long long expire_at = fetched_at + CACHE_TTL_MS;
    memcpy(copy, data, len);
    copy[len] = '\0';

    pthread_mutex_lock(&cache_lock);
    long long now = cache_clock();
    /*
     * 같은 key의 사본이 있으면 새 사본으로 바꾼다.
     * cache_get이 miss를 돌려준 뒤 여기까지 오는 동안 잠금이 풀려 있었으므로, 그때 본 것을 믿지 않고 잠금 안에서 key로 다시 찾는다.
     * 그 사이 다른 스레드가 같은 key를 넣었어도 사본은 하나만 남는다.
     */
    struct cache_entry *entry = find_entry(key);
    /*
     * 저장하지 않는 두 경우:
     * - 받는 데 TTL보다 오래 걸려서 이미 만료된 응답
     * - 있는 사본이 더 늦게 물어본 응답일 때 (expire_at이 더 늦다). 먼저 물어보고 늦게 끝난 스레드가 새 사본을 옛 내용으로 덮어쓰지 않는다
     */
    if (now >= expire_at || (entry != NULL && now < entry->expire_at && entry->expire_at > expire_at)) {
        pthread_mutex_unlock(&cache_lock);
        free(copy);
        return 0;
    }
    if (entry != NULL) {
        remove_entry(entry);
    }
    /* 전체 크기 제한을 넘거나 빈 칸이 없으면 사본을 하나씩 버린다. 만료된 사본이 먼저다 */
    /* 버린 사본의 로그는 잠금을 푼 뒤에 출력한다. 출력이 막혀도 cache_lock을 기다리는 스레드가 같이 멈추지 않는다 */
    char *dropped_keys[CACHE_SLOTS];
    int dropped_expired[CACHE_SLOTS];
    int dropped = 0;
    while (total_size + len > CACHE_TOTAL_MAX || find_empty() == NULL) {
        struct cache_entry *victim = pick_victim(now);
        dropped_keys[dropped] = strdup(victim->key);
        dropped_expired[dropped] = now >= victim->expire_at;
        dropped++;
        remove_entry(victim);
    }
    entry = find_empty();
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    entry->data = copy;
    entry->len = len;
    entry->expire_at = expire_at;
    entry->last_used = ++use_count;
    total_size += len;
    pthread_mutex_unlock(&cache_lock);

    for (int i = 0; i < dropped; i++) {
        if (dropped_keys[i] != NULL) {
            printf("[Cache] 버림 %s (%s)\n", dropped_keys[i], dropped_expired[i] ? "만료됨" : "가장 오래 쓰지 않음");
            free(dropped_keys[i]);
        }
    }
    return 1;
}

void cache_clear(void)
{
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (entries[i].data != NULL) {
            remove_entry(&entries[i]);
        }
    }
    pthread_mutex_unlock(&cache_lock);
}

size_t cache_size(void)
{
    pthread_mutex_lock(&cache_lock);
    size_t size = total_size;
    pthread_mutex_unlock(&cache_lock);
    return size;
}
