#!/usr/bin/env bash
# CS:APP 11장 예제 동작 확인. 사용법: make test (또는 tiny/ 안에서 bash tests.sh [시작 포트])
#   Tiny: 정적 HTML·GIF, adder(CGI), 404, 403, 501
#   에코 서버·클라이언트: 보낸 줄이 그대로 돌아오는지
#   hostinfo: localhost가 127.0.0.1로 풀리는지
set -u
cd "$(dirname "$0")"

port="${1:-18300}"
echo_port=$((port + 1))
fail=0
pids=()
tmp=$(mktemp -d)
# 서버들은 무한 루프라 스크립트가 끝날 때 직접 끈다
trap 'kill "${pids[@]}" 2>/dev/null; rm -rf "$tmp"' EXIT

pass() { echo "PASS: $1"; }
failed() { echo "FAIL: $1"; fail=1; }
check() { if eval "$2"; then pass "$1"; else failed "$1"; fi; }

# 서버가 응답할 때까지 최대 약 2초 기다린다. 인자: 연결을 시도할 명령
# 주의: Tiny는 요청 없이 닫히는 빈 연결을 받으면 SIGPIPE로 죽는다(책 11.6 Aside).
#       그래서 빈 연결로 포트만 확인하지 않고 실제 요청을 보내 본다.
wait_ready() {
  for _ in $(seq 20); do
    "$@" > /dev/null 2>&1 && return 0
    sleep 0.1
  done
  return 1
}

# stdbuf -oL: 서버 printf가 파일로 갈 때도 줄마다 바로 기록되게 (나중에 kill로 끝내므로)
stdbuf -oL ./tiny "$port" > "$tmp/tiny.log" 2>&1 &
pids+=($!)
url="http://127.0.0.1:$port"
wait_ready curl -s -o /dev/null "$url/" || { failed "tiny가 포트 $port에서 응답"; exit 1; }
# 포트를 이미 다른 프로세스가 잡고 있으면 우리 tiny는 Open_listenfd 에러로 끝나고 응답은 그 프로세스가 한다
kill -0 "${pids[0]}" 2>/dev/null || { failed "tiny가 살아 있음 (포트 $port를 다른 프로세스가 쓰는 중일 수 있음: $(cat "$tmp/tiny.log"))"; exit 1; }

# 정적 콘텐츠: / → ./home.html
code=$(curl -s -o "$tmp/home" -D "$tmp/home.h" -w '%{http_code}' "$url/")
check "GET / → 200" '[ "$code" = 200 ]'
check "GET / 본문이 home.html과 같음" 'cmp -s "$tmp/home" home.html'
check "GET / Content-type: text/html" 'grep -q "^Content-type: text/html" "$tmp/home.h"'
check "GET / Content-length가 파일 크기" 'grep -qx "Content-length: $(stat -c %s home.html)"$'"'"'\r'"'"' "$tmp/home.h"'

# 정적 콘텐츠: 이미지 (mmap으로 보낸 바이트가 파일과 같은지)
code=$(curl -s -o "$tmp/gif" -D "$tmp/gif.h" -w '%{http_code}' "$url/godzilla.gif")
check "GET /godzilla.gif → 200" '[ "$code" = 200 ]'
check "GET /godzilla.gif 본문이 파일과 같음" 'cmp -s "$tmp/gif" godzilla.gif'
check "GET /godzilla.gif Content-type: image/gif" 'grep -q "^Content-type: image/gif" "$tmp/gif.h"'

# 동적 콘텐츠: 책 그림 11.28과 같은 요청
code=$(curl -s -o "$tmp/add" -D "$tmp/add.h" -w '%{http_code}' "$url/cgi-bin/adder?15000&213")
check "GET /cgi-bin/adder?15000&213 → 200" '[ "$code" = 200 ]'
check "adder 본문에 15000 + 213 = 15213" 'grep -q "The answer is: 15000 + 213 = 15213" "$tmp/add"'
check "adder Content-length: 115 (책 그림 11.28)" 'grep -qx "Content-length: 115"$'"'"'\r'"'"' "$tmp/add.h"'
check "adder 본문 크기가 115바이트" '[ "$(stat -c %s "$tmp/add")" = 115 ]'

# 에러 응답
code=$(curl -s -o /dev/null -w '%{http_code}' "$url/no-such-file.html")
check "없는 파일 → 404" '[ "$code" = 404 ]'
code=$(curl -s -o "$tmp/403" -w '%{http_code}' "$url/cgi-bin/adder.c")
check "실행 권한 없는 cgi-bin 파일 → 403" '[ "$code" = 403 ]'
check "403 본문에 Tiny Error 페이지" 'grep -q "Tiny couldn.t run the CGI program" "$tmp/403"'
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$url/")
check "POST → 501" '[ "$code" = 501 ]'

# 에코 서버·클라이언트
stdbuf -oL ./echoserveri "$echo_port" > "$tmp/echo.log" 2>&1 &
pids+=($!)
printf 'hello\nworld\n' > "$tmp/echo.in"
# 에코 서버는 빈 연결도 EOF로 정상 처리하므로 /dev/tcp 빈 연결로 준비를 확인한다
# (echoclient로 확인하면 안 됨: csapp 래퍼는 연결에 실패해도 exit(0)으로 끝난다)
wait_ready bash -c "exec 3<>/dev/tcp/127.0.0.1/$echo_port" || failed "echoserveri가 포트 $echo_port에서 응답"
kill -0 "${pids[1]}" 2>/dev/null || failed "echoserveri가 살아 있음 (포트 $echo_port를 다른 프로세스가 쓰는 중일 수 있음)"
./echoclient localhost "$echo_port" < "$tmp/echo.in" > "$tmp/echo.out"
check "echoclient 출력이 입력과 같음" 'cmp -s "$tmp/echo.in" "$tmp/echo.out"'
check "echoserveri가 6바이트씩 두 번 받음" '[ "$(grep -c "server received 6 bytes" "$tmp/echo.log")" = 2 ]'

# hostinfo
check "hostinfo localhost에 127.0.0.1" './hostinfo localhost | grep -qx 127.0.0.1'

exit "$fail"
