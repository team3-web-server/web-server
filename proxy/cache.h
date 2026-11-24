#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

/*
 * 응답 캐시: 프록시 프로세스의 메모리에 백엔드 응답의 사본(상태 줄, 헤더, 본문 바이트 그대로)을 둔다.
 * 같은 요청이 다시 오면 백엔드에 연결하지 않고 사본을 보낸다.
 * 여러 스레드가 함께 쓰므로 안에서 잠금(cache_lock)을 잡는다.
 */

/*
 * 사본을 쓰는 기간: 저장한 때부터 10초.
 * 백엔드의 내용이 바뀌어도 프록시는 알 수 없고, 사본을 새것으로 바꾸는 수단은 만료뿐이다.
 * 길면 백엔드에 덜 가지만 예전 내용을 오래 보내고, 짧으면 그 반대다.
 */
#define CACHE_TTL_MS 10000
#define CACHE_OBJECT_MAX (100 * 1024)   /* 사본 하나의 최대 크기 100 KiB. 넘는 응답은 저장하지 않는다 */
#define CACHE_TOTAL_MAX (1024 * 1024)   /* 사본 전체의 최대 크기 1 MiB. 넘치면 사본을 버려 자리를 만든다 */
#define CACHE_SLOTS 64                  /* 사본을 두는 칸의 수 */
#define CACHE_KEY_SIZE 1024             /* 키(요청 대상)의 최대 길이. 넘는 요청은 저장하지 않는다 */

/*
 * 캐시가 지금 시각(밀리초)을 알아낼 때 부르는 함수. 처음에는 CLOCK_MONOTONIC을 읽는 함수를 가리킨다.
 * 테스트는 이 포인터를 원하는 시각을 돌려주는 함수로 바꿔서, 10초를 기다리지 않고 만료를 확인한다.
 */
extern long long (*cache_clock)(void);

/*
 * key의 사본을 찾는다. 있고 만료되지 않았으면 사본을 새로 malloc한 메모리에 복사해
 * *data, *len에 넣고 1을 돌려준다 (hit). 부른 쪽이 free한다. 없거나 만료됐으면 0 (miss).
 * 만료 시각이 된 순간부터 만료다 (지금 >= 만료 시각). 만료된 사본은 찾은 그 자리에서 지운다 (지연 삭제).
 * hit은 "마지막으로 쓴 순번"만 바꾸고 만료 시각은 늘리지 않는다.
 */
int cache_get(const char *key, char **data, size_t *len);

/*
 * key의 사본으로 data(len바이트)를 저장한다. 만료 시각은 저장한 때 + CACHE_TTL_MS.
 * 같은 key의 사본이 있으면 바꾼다.
 * 자리가 모자라면 만료된 사본부터, 그다음은 가장 오래 쓰지 않은 사본부터 버린다 (LRU).
 * len이 CACHE_OBJECT_MAX보다 크거나 key가 너무 길면 저장하지 않는다.
 */
void cache_put(const char *key, const char *data, size_t len);

/* 사본을 모두 버린다 (테스트에서 쓴다) */
void cache_clear(void);

/* 지금 들어 있는 사본 크기의 합 (테스트에서 쓴다) */
size_t cache_size(void);

#endif /* CACHE_H */
