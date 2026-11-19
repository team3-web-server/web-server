# =====================================================================
# 웹 서버 빌드 규칙
#   make        : build/server 빌드 (-Wall -Wextra 경고 확인)
#   make test   : 빌드 후 tests/run_tests.sh 실행
#   make asan   : -fsanitize=address,leak 로 build/server-asan 빌드 후 테스트
#   make clean  : build/ 삭제
# =====================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -g -Iinclude
ASANFLAGS := -fsanitize=address,leak -fno-omit-frame-pointer

SRCS    := $(wildcard src/*.c)
BUILD   := build
BIN     := $(BUILD)/server
ASANBIN := $(BUILD)/server-asan

.PHONY: all test asan clean

all: $(BIN)

$(BIN): $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) -o $@

$(ASANBIN): $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $(ASANFLAGS) $(SRCS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(BIN)
	bash tests/run_tests.sh $(BIN)

asan: $(ASANBIN)
	bash tests/run_tests.sh $(ASANBIN)

clean:
	rm -rf $(BUILD)
