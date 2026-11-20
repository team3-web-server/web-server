# CS:APP 11장 예제 빌드 (책 배포본 tiny/Makefile과 같은 옵션: -O2 -Wall -I .)
#   make        : tiny, cgi-bin/adder, echoclient, echoserveri, hostinfo 빌드
#   make test   : tests.sh로 Tiny·adder·에코·hostinfo 동작 확인
#   make clean  : 빌드 결과물 삭제
CC = gcc
CFLAGS = -O2 -Wall -I .

PROGS = tiny cgi-bin/adder echoclient echoserveri hostinfo

all: $(PROGS)

tiny: tiny.c netlib.o
	$(CC) $(CFLAGS) -o tiny tiny.c netlib.o

cgi-bin/adder: cgi-bin/adder.c netlib.o
	$(CC) $(CFLAGS) -o cgi-bin/adder cgi-bin/adder.c netlib.o

echoclient: echoclient.c netlib.o
	$(CC) $(CFLAGS) -o echoclient echoclient.c netlib.o

echoserveri: echoserveri.c echo.c netlib.o
	$(CC) $(CFLAGS) -o echoserveri echoserveri.c echo.c netlib.o

hostinfo: hostinfo.c netlib.o
	$(CC) $(CFLAGS) -o hostinfo hostinfo.c netlib.o

netlib.o: netlib.c netlib.h
	$(CC) $(CFLAGS) -c netlib.c

test: all
	bash tests.sh

clean:
	rm -f *.o $(PROGS) *~

.PHONY: all test clean
