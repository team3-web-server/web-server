# CS:APP 11장 예제 빌드 (책 배포본 tiny/Makefile과 같은 옵션: -O2 -Wall)
#   make        : cgi-bin/adder, echoclient, echoserveri, hostinfo 빌드
#   make test   : ../build/tiny를 빌드한 뒤 tests.sh로 Tiny·adder·에코·hostinfo 동작 확인
#   make clean  : 빌드 결과물 삭제
# 서버 소스(tiny.c, netlib.c)는 ../src, 헤더는 ../include로 옮겼다.
CC = gcc
CFLAGS = -O2 -Wall -I ../include

PROGS = cgi-bin/adder echoclient echoserveri hostinfo

all: $(PROGS)

cgi-bin/adder: cgi-bin/adder.c ../src/netlib.c
	$(CC) $(CFLAGS) -o cgi-bin/adder cgi-bin/adder.c ../src/netlib.c

echoclient: echoclient.c ../src/netlib.c
	$(CC) $(CFLAGS) -o echoclient echoclient.c ../src/netlib.c

echoserveri: echoserveri.c echo.c ../src/netlib.c
	$(CC) $(CFLAGS) -o echoserveri echoserveri.c echo.c ../src/netlib.c

hostinfo: hostinfo.c ../src/netlib.c
	$(CC) $(CFLAGS) -o hostinfo hostinfo.c ../src/netlib.c

test: all
	$(MAKE) -C .. build/tiny
	bash tests.sh

clean:
	rm -f *.o $(PROGS) *~

.PHONY: all test clean
