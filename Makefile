# =====================================================================
# 웹 서버 빌드 규칙
#   make        : build/server 빌드 (-Wall -Wextra 경고 확인)
#   make test   : 서버와 단위 테스트를 빌드한 뒤 tests/run_tests.sh 실행
#   make asan   : -fsanitize=address,leak 로 같은 테스트를 빌드·실행
#   make demo   : demo/ 의 시연용 프로그램 빌드 (build/listen_demo, build/connect_demo)
#   make clean  : build/ 삭제
# =====================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -g -Iinclude
ASANFLAGS := -fsanitize=address,leak -fno-omit-frame-pointer

SRCS    := $(wildcard src/*.c)
HDRS    := $(wildcard include/*.h)
# 단위 테스트는 main.c를 빼고 링크한다 (테스트 파일이 자기 main을 가지므로)
LIBSRCS := $(filter-out src/main.c,$(SRCS))
TESTSRCS := tests/test_net.c

BUILD   := build
BIN     := $(BUILD)/server
ASANBIN := $(BUILD)/server-asan
TESTBIN := $(BUILD)/test_net
ASANTESTBIN := $(BUILD)/test_net-asan

.PHONY: all test asan demo clean

all: $(BIN)

$(BIN): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) -o $@

$(ASANBIN): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ASANFLAGS) $(SRCS) -o $@

$(TESTBIN): $(LIBSRCS) $(TESTSRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LIBSRCS) $(TESTSRCS) -o $@

$(ASANTESTBIN): $(LIBSRCS) $(TESTSRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ASANFLAGS) $(LIBSRCS) $(TESTSRCS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(BIN) $(TESTBIN)
	bash tests/run_tests.sh $(BIN) $(TESTBIN)

asan: $(ASANBIN) $(ASANTESTBIN)
	bash tests/run_tests.sh $(ASANBIN) $(ASANTESTBIN)

# 시연용 프로그램: 서버 본체(src/)와 따로 빌드한다. 코드 리뷰 때 대기열·블로킹 같은 상황을 보여 주는 용도
demo: $(BUILD)/listen_demo $(BUILD)/connect_demo

$(BUILD)/listen_demo: demo/listen_demo.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/connect_demo: demo/connect_demo.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf $(BUILD)
