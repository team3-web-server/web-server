# =====================================================================
# 웹 서버 빌드 규칙 (junho/: 준호의 단계별 구현, 이 폴더 안에서 make를 실행한다)
#   make            : build/server(단계 1)와 build/tiny(HTTP 서버) 빌드
#   make test       : 서버와 단위 테스트를 빌드한 뒤 tests/run_tests.sh 실행
#   make tiny-test  : build/tiny를 빌드한 뒤 tiny/tests.sh 실행
#   make asan       : -fsanitize=address,leak 로 같은 테스트를 빌드·실행
#   make clean      : build/ 삭제
# =====================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -g -Iinclude
# tiny 쪽은 책 예제 그대로 _POSIX_C_SOURCE를 정의하지 않으므로 -std=c11을 쓰지 않는다
TINYCFLAGS := -O2 -Wall -Wextra -Iinclude
ASANFLAGS := -fsanitize=address,leak -fno-omit-frame-pointer

# 두 프로그램의 main이 한 링크에 들어가지 않도록 소스 집합을 나눈다
SERVER_SRCS := src/step1_main.c src/net.c
TINY_SRCS   := src/tiny.c src/netlib.c
HDRS    := $(wildcard include/*.h)
# 단위 테스트는 두 main과 netlib(자체 open_listenfd 포함)을 빼고 링크한다
LIBSRCS := src/net.c
TESTSRCS := tests/test_net.c

BUILD   := build
BIN     := $(BUILD)/server
TINYBIN := $(BUILD)/tiny
ASANBIN := $(BUILD)/server-asan
TESTBIN := $(BUILD)/test_net
ASANTESTBIN := $(BUILD)/test_net-asan

.PHONY: all test tiny-test asan clean

all: $(BIN) $(TINYBIN)

$(BIN): $(SERVER_SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SERVER_SRCS) -o $@

$(TINYBIN): $(TINY_SRCS) $(HDRS) | $(BUILD)
	$(CC) $(TINYCFLAGS) $(TINY_SRCS) -o $@

$(ASANBIN): $(SERVER_SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ASANFLAGS) $(SERVER_SRCS) -o $@

$(TESTBIN): $(LIBSRCS) $(TESTSRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LIBSRCS) $(TESTSRCS) -o $@

$(ASANTESTBIN): $(LIBSRCS) $(TESTSRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ASANFLAGS) $(LIBSRCS) $(TESTSRCS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(BIN) $(TESTBIN)
	bash tests/run_tests.sh $(BIN) $(TESTBIN)

tiny-test: $(TINYBIN)
	$(MAKE) -C tiny test

asan: $(ASANBIN) $(ASANTESTBIN)
	bash tests/run_tests.sh $(ASANBIN) $(ASANTESTBIN)

clean:
	rm -rf $(BUILD)
