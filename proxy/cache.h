#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

/*
 * 응답 캐시: 프록시 프로세스의 메모리에 백엔드 응답의 사본(상태 줄, 헤더, 본문 바이트 그대로)을 둔다.
 * 같은 요청이 다시 오면 백엔드에 연결하지 않고 사본을 보낸다.
 * 여러 스레드가 함께 쓰므로 안에서 잠금(cache_lock)을 잡는다.
 */

#define CACHE_TTL_MS 10000              /* 사본을 쓰는 기간 10초 */
#define CACHE_OBJECT_MAX (100 * 1024)   /* 사본 하나의 최대 크기 100 KiB. 넘는 응답은 저장하지 않는다 */
#define CACHE_TOTAL_MAX (1024 * 1024)   /* 사본 전체의 최대 크기 1 MiB. 넘치면 사본을 버려 자리를 만든다 */
#define CACHE_SLOTS 64                  /* 사본을 두는 칸의 수 */
#define CACHE_KEY_SIZE 1024             /* 키(요청 대상)의 최대 길이. 넘는 요청은 저장하지 않는다 */

/*
 * key의 사본을 찾는다. 있고 만료되지 않았으면 사본을 새로 malloc한 메모리에 복사해
 * *data, *len에 넣고 1을 돌려준다 (hit). 부른 쪽이 free한다. 없거나 만료됐으면 0 (miss).
 */
int cache_get(const char *key, char **data, size_t *len);

/*
 * key의 사본으로 data(len바이트)를 저장한다. 같은 key의 사본이 있으면 바꾼다.
 * 자리가 모자라면 가장 오래 쓰지 않은 사본부터 버린다 (LRU).
 * len이 CACHE_OBJECT_MAX보다 크거나 key가 너무 길면 저장하지 않는다.
 */
void cache_put(const char *key, const char *data, size_t len);

/* 사본을 모두 버린다 (테스트에서 쓴다) */
void cache_clear(void);

#endif /* CACHE_H */
