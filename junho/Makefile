# =====================================================================
# 과제용 웹 서버 빌드 규칙 (저장소 루트에서 make를 실행한다)
#   make        : build/server와 www/cgi-bin의 CGI 프로그램 빌드
#   make test   : 서버를 빌드한 뒤 tests/server_test.sh 실행
#   make clean  : build/와 CGI 실행 파일 삭제
# =====================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -g -Iinclude -D_POSIX_C_SOURCE=200809L
BUILD   := build
BIN     := $(BUILD)/server
SRCS    := src/server_main.c src/http.c src/serve.c src/netlib.c
HDRS    := $(wildcard include/*.h)
CGIBIN  := www/cgi-bin/adder www/cgi-bin/postecho

.PHONY: all test clean

all: $(BIN) $(CGIBIN)

$(BIN): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) -o $@

www/cgi-bin/adder: www/cgi-bin/adder.c
	$(CC) $(CFLAGS) $< -o $@

www/cgi-bin/postecho: www/cgi-bin/postecho.c
	$(CC) $(CFLAGS) $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: all
	bash tests/server_test.sh $(BIN)

clean:
	rm -rf $(BUILD)
	rm -f $(CGIBIN)
