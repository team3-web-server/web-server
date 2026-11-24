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
    long long saved_at;  /* 저장한 시각(밀리초): 만료(TTL)를 판단한다. hit해도 바뀌지 않는다 */
    long long last_used; /* 마지막으로 쓴 시각(밀리초): 버릴 사본(LRU)을 고른다 */
};

static struct cache_entry entries[CACHE_SLOTS];
static size_t total_size; /* 사본 크기의 합 */

/* entries, total_size를 읽고 쓰는 동안 잡는 잠금 */
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

/* 지금 시각(밀리초). 흐른 시간을 재는 시계(CLOCK_MONOTONIC)라서 컴퓨터의 날짜와 시각을 바꿔도 거꾸로 가지 않는다 */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

/* 가장 오래 쓰지 않은 사본 (last_used가 가장 작은 것). 사본이 하나도 없으면 NULL */
static struct cache_entry *find_oldest(void)
{
    struct cache_entry *oldest = NULL;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (entries[i].data != NULL && (oldest == NULL || entries[i].last_used < oldest->last_used)) {
            oldest = &entries[i];
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
    long long now = now_ms();
    struct cache_entry *entry = find_entry(key);
    if (entry != NULL && now - entry->saved_at <= CACHE_TTL_MS) {
        /* 잠금을 푼 뒤에는 이 사본이 버려질 수 있으므로, 잠금 안에서 복사해 둔다 */
        char *copy = malloc(entry->len + 1);
        if (copy != NULL) {
            memcpy(copy, entry->data, entry->len);
            copy[entry->len] = '\0';
            *data = copy;
            *len = entry->len;
            entry->last_used = now;
            hit = 1;
        }
    }
    pthread_mutex_unlock(&cache_lock);
    return hit;
}

void cache_put(const char *key, const char *data, size_t len)
{
    if (len > CACHE_OBJECT_MAX || strlen(key) >= CACHE_KEY_SIZE) {
        return;
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, data, len);
    copy[len] = '\0';

    pthread_mutex_lock(&cache_lock);
    /* 같은 key의 사본이 있으면 새 사본으로 바꾼다. 잠금 안에서 key로 다시 찾으므로 다른 스레드가 먼저 넣었어도 하나만 남는다 */
    struct cache_entry *entry = find_entry(key);
    if (entry != NULL) {
        remove_entry(entry);
    }
    /* 전체 크기 제한을 넘거나 빈 칸이 없으면, 가장 오래 쓰지 않은 사본부터 하나씩 버린다 */
    while (total_size + len > CACHE_TOTAL_MAX || find_empty() == NULL) {
        struct cache_entry *oldest = find_oldest();
        printf("[Cache] 버림 %s (가장 오래 쓰지 않음)\n", oldest->key);
        remove_entry(oldest);
    }
    entry = find_empty();
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    entry->data = copy;
    entry->len = len;
    entry->saved_at = now_ms();
    entry->last_used = entry->saved_at;
    total_size += len;
    pthread_mutex_unlock(&cache_lock);
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
