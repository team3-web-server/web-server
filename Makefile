# =====================================================================
# 웹 서버 빌드 규칙
#   make        : build/server와 CGI 예제 프로그램(www/cgi-bin/hello, www/cgi-bin/echo) 빌드 (-Wall -Wextra 경고 확인)
#   make test   : 서버와 단위 테스트(build/test_net, build/test_http, build/test_response)를 빌드한 뒤 tests/run_tests.sh 실행
#   make asan   : -fsanitize=address,leak 로 같은 테스트를 빌드·실행
#   make demo   : demo/ 의 시연용 프로그램 빌드 (build/listen_demo, build/connect_demo)
#   make clean  : build/ 와 www/cgi-bin/ 삭제
# =====================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -g -Iinclude
ASANFLAGS := -fsanitize=address,leak -fno-omit-frame-pointer

SRCS    := $(wildcard src/*.c)
HDRS    := $(wildcard include/*.h)
# 단위 테스트는 main.c를 빼고 링크한다 (테스트 파일이 자기 main을 가지므로)
LIBSRCS := $(filter-out src/main.c,$(SRCS))

BUILD   := build
BIN     := $(BUILD)/server
ASANBIN := $(BUILD)/server-asan
# 단위 테스트 실행 파일: tests/test_net.c → build/test_net, tests/test_http.c → build/test_http
TESTBINS := $(BUILD)/test_net $(BUILD)/test_http $(BUILD)/test_response
ASANTESTBINS := $(BUILD)/test_net-asan $(BUILD)/test_http-asan $(BUILD)/test_response-asan
# CGI 예제 프로그램: cgi/hello.c → www/cgi-bin/hello. 웹 서버 프로세스가 요청을 받으면 실행하는 별도의 프로그램이다
CGIDIR  := www/cgi-bin
CGIBINS := $(CGIDIR)/hello $(CGIDIR)/echo

.PHONY: all test asan demo clean

all: $(BIN) $(CGIBINS)

$(BIN): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) -o $@

$(ASANBIN): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ASANFLAGS) $(SRCS) -o $@

# 패턴 규칙: %는 같은 글자열을 뜻한다. build/test_http 를 만들 때는 tests/test_http.c 를 쓴다
# $< 는 첫 번째 재료(tests/test_*.c), $@ 는 만들 파일 이름
$(BUILD)/test_%: tests/test_%.c $(LIBSRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LIBSRCS) $< -o $@

$(BUILD)/test_%-asan: tests/test_%.c $(LIBSRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ASANFLAGS) $(LIBSRCS) $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

# CGI 프로그램은 서버와 따로 빌드되는 독립된 실행 파일이다 (src/ 의 코드를 쓰지 않는다)
$(CGIDIR)/%: cgi/%.c | $(CGIDIR)
	$(CC) $(CFLAGS) $< -o $@

$(CGIDIR):
	mkdir -p $(CGIDIR)

test: $(BIN) $(CGIBINS) $(TESTBINS)
	bash tests/run_tests.sh $(BIN) $(TESTBINS)

asan: $(ASANBIN) $(CGIBINS) $(ASANTESTBINS)
	bash tests/run_tests.sh $(ASANBIN) $(ASANTESTBINS)

# 시연용 프로그램: 서버 본체(src/)와 따로 빌드한다. 코드 리뷰 때 대기열·블로킹 같은 상황을 보여 주는 용도
demo: $(BUILD)/listen_demo $(BUILD)/connect_demo

$(BUILD)/listen_demo: demo/listen_demo.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/connect_demo: demo/connect_demo.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf $(BUILD) $(CGIDIR)
