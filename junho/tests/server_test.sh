#!/usr/bin/env bash
# 과제 서버 통합 테스트. 사용법: make test (또는 bash tests/server_test.sh [build/server] [포트])
# 서버는 www/에서 실행해야 정적 파일과 cgi-bin을 찾는다.
set -u

bin="${1:-build/server}"
if [ -x "$bin" ]; then
  bin="$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")"
fi
cd "$(dirname "$0")/../www"
port="${2:-18611}"
url="http://127.0.0.1:$port"
fail=0
tmp=$(mktemp -d)
pids=()

pass() { echo "PASS: $1"; }
failed() { echo "FAIL: $1"; fail=1; }
check() {
  local desc="$1"
  shift
  if "$@"; then pass "$desc"; else failed "$desc"; fi
}

# 응답이 헤더 끝(\r\n\r\n)에서 바로 끝나면 성공.
no_body() {
  python3 - "$1" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
i = data.find(b"\r\n\r\n")
sys.exit(0 if i >= 0 and len(data) == i + 4 else 1)
PY
}

cleanup() {
  kill "${pids[@]}" 2>/dev/null
  rm -rf "$tmp"
  rm -f big.bin empty.bin style.css app.js
}
trap cleanup EXIT

# 큰 파일과 0바이트 파일을 만든다.
dd if=/dev/urandom of=big.bin bs=1000 count=100 2>/dev/null
: > empty.bin
printf 'body { color: #000; }\n' > style.css
printf 'console.log("hi");\n' > app.js

stdbuf -oL "$bin" "$port" > "$tmp/server.log" 2>&1 &
pids+=($!)
pid=${pids[0]}
ready=0
for _ in $(seq 50); do
  if curl -s -o /dev/null --max-time 1 "$url/"; then
    ready=1
    break
  fi
  sleep 0.1
done
if [ "$ready" -ne 1 ] || ! kill -0 "$pid" 2>/dev/null; then
  echo "FAIL: 서버가 포트 $port에서 응답하지 않음"
  cat "$tmp/server.log"
  exit 1
fi

echo "== 요청 줄·헤더 출력과 HTTP 버전 =="
check "서버 로그에 요청 줄이 있음" grep -q 'Request line: GET / HTTP/1.1' "$tmp/server.log"
check "서버 로그에 Host 헤더가 있음" grep -q 'Request header: Host:' "$tmp/server.log"
check "서버 로그에 User-Agent 헤더가 있음" grep -q 'Request header: User-Agent:' "$tmp/server.log"
check "클라이언트가 HTTP/1.1로 요청함" grep -q 'HTTP/1.1' "$tmp/server.log"

echo "== 정적 콘텐츠와 MIME =="
code=$(curl -s -o "$tmp/home" -D "$tmp/home.h" -w '%{http_code}' "$url/")
check "GET / → 200" test "$code" = 200
check "GET / 본문이 home.html과 같음" cmp -s "$tmp/home" home.html
check "GET / Content-type: text/html" grep -qi '^Content-type: text/html' "$tmp/home.h"
check "GET / Content-length가 파일 크기" grep -qi "^Content-length: $(stat -c %s home.html)" "$tmp/home.h"

code=$(curl -s -o "$tmp/gif" -D "$tmp/gif.h" -w '%{http_code}' "$url/godzilla.gif")
check "GET /godzilla.gif → 200" test "$code" = 200
check "GET /godzilla.gif 본문 일치" cmp -s "$tmp/gif" godzilla.gif
check "GET /godzilla.gif Content-type: image/gif" grep -qi '^Content-type: image/gif' "$tmp/gif.h"

code=$(curl -s -o "$tmp/css" -D "$tmp/css.h" -w '%{http_code}' "$url/style.css")
check "GET /style.css → 200" test "$code" = 200
check "GET /style.css Content-type: text/css" grep -qi '^Content-type: text/css' "$tmp/css.h"

code=$(curl -s -o "$tmp/js" -D "$tmp/js.h" -w '%{http_code}' "$url/app.js")
check "GET /app.js → 200" test "$code" = 200
check "GET /app.js Content-type: text/javascript" grep -qi '^Content-type: text/javascript' "$tmp/js.h"

code=$(curl -s -o "$tmp/mpg" -D "$tmp/mpg.h" -w '%{http_code}' "$url/sample.mpg")
check "GET /sample.mpg → 200" test "$code" = 200
check "GET /sample.mpg 본문 일치" cmp -s "$tmp/mpg" sample.mpg
check "GET /sample.mpg Content-type: video/mpeg" grep -qi '^Content-type: video/mpeg' "$tmp/mpg.h"

code=$(curl -s -o "$tmp/big" -w '%{http_code}' "$url/big.bin")
check "GET /big.bin → 200" test "$code" = 200
check "GET /big.bin 본문 일치 (malloc+rio_readn)" cmp -s "$tmp/big" big.bin

code=$(curl -s -o "$tmp/empty" -D "$tmp/empty.h" -w '%{http_code}' "$url/empty.bin")
check "GET /empty.bin → 200" test "$code" = 200
check "GET /empty.bin 본문 0바이트" test ! -s "$tmp/empty"
check "GET /empty.bin Content-length: 0" grep -qi '^Content-length: 0' "$tmp/empty.h"

echo "== CGI와 GET 폼 =="
code=$(curl -s -o "$tmp/add" -D "$tmp/add.h" -w '%{http_code}' "$url/cgi-bin/adder?1&2")
check "GET /cgi-bin/adder?1&2 → 200" test "$code" = 200
check "adder가 1 + 2 = 3을 출력" grep -q 'The answer is: 1 + 2 = 3' "$tmp/add"

code=$(curl -s -o "$tmp/add2" -w '%{http_code}' "$url/cgi-bin/adder?num1=15000&num2=213")
check "GET /cgi-bin/adder?num1=15000&num2=213 → 200" test "$code" = 200
check "폼 형식 쿼리로 15000 + 213 = 15213" grep -q 'The answer is: 15000 + 213 = 15213' "$tmp/add2"

code=$(curl -s -o "$tmp/adder1" -w '%{http_code}' "$url/cgi-bin/adder?5")
check "GET /cgi-bin/adder?5 → 200 (& 없이 크래시하지 않음)" test "$code" = 200
check "& 없는 쿼리는 5 + 0 = 5" grep -q 'The answer is: 5 + 0 = 5' "$tmp/adder1"

code=$(curl -s -o "$tmp/form" -w '%{http_code}' "$url/adder.html")
check "GET /adder.html → 200" test "$code" = 200
check "폼이 GET으로 /cgi-bin/adder에 제출" grep -q 'action="/cgi-bin/adder" method="get"' "$tmp/form"

echo "== 오류 응답 =="
code=$(curl -s -o /dev/null -w '%{http_code}' "$url/no-such-file.html")
check "없는 파일 → 404" test "$code" = 404
code=$(curl -s -o /dev/null -w '%{http_code}' "$url/cgi-bin/adder.c")
check "실행 권한 없는 CGI → 403" test "$code" = 403
code=$(curl -s --path-as-is -o /dev/null -w '%{http_code}' "$url/../src/server_main.c")
check "문서 루트 밖 경로 → 403" test "$code" = 403
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT "$url/")
check "PUT → 501" test "$code" = 501
code=$(printf 'GET / HTTP/2.0\r\nHost: x\r\n\r\n' | timeout 3 nc 127.0.0.1 "$port" | head -1)
check "HTTP/2.0 요청 → 505" grep -q '505' <<< "$code"

echo "== HEAD =="
printf 'HEAD / HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 3 nc 127.0.0.1 "$port" > "$tmp/head"
check "HEAD / → 200" grep -q '^HTTP/1.0 200 OK' "$tmp/head"
check "HEAD / Content-length가 파일 크기" grep -qi "^Content-length: $(stat -c %s home.html)" "$tmp/head"
check "HEAD / 응답에 본문이 없음" no_body "$tmp/head"

printf 'HEAD /cgi-bin/adder?1&2 HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 3 nc 127.0.0.1 "$port" > "$tmp/headcgi"
check "HEAD CGI → 200" grep -q '^HTTP/1.0 200 OK' "$tmp/headcgi"
check "HEAD CGI Content-length가 있음" grep -qi '^Content-length: [0-9][0-9]*' "$tmp/headcgi"
check "HEAD CGI 응답에 본문이 없음" no_body "$tmp/headcgi"

printf 'HEAD /no-such HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 3 nc 127.0.0.1 "$port" > "$tmp/head404"
check "HEAD 없는 파일 → 404" grep -q '^HTTP/1.0 404 Not Found' "$tmp/head404"
check "HEAD 404 응답에 본문이 없음" no_body "$tmp/head404"

echo "== POST =="
code=$(curl -s -o "$tmp/post" -D "$tmp/post.h" -w '%{http_code}' \
  -X POST --data-binary 'hello=world' "$url/cgi-bin/postecho")
check "POST /cgi-bin/postecho → 200" test "$code" = 200
check "POST 본문이 CGI 표준 입력으로 전달됨" test "$(cat "$tmp/post")" = 'hello=world'
check "POST 응답 Content-type: text/plain" grep -qi '^Content-type: text/plain' "$tmp/post.h"
check "POST 응답 Content-length: 11" grep -qi '^Content-length: 11' "$tmp/post.h"

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d 'x=1' "$url/")
check "정적 경로에 POST → 405" test "$code" = 405

printf 'POST /cgi-bin/postecho HTTP/1.1\r\nHost: x\r\n\r\n' | timeout 3 nc 127.0.0.1 "$port" > "$tmp/nocl"
check "Content-Length 없는 POST → 400" grep -q '400' "$tmp/nocl"

printf 'POST /cgi-bin/postecho HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n' \
  | timeout 3 nc 127.0.0.1 "$port" > "$tmp/te"
check "Transfer-Encoding 요청 → 501" grep -q '501' "$tmp/te"

printf 'POST /cgi-bin/postecho HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n' \
  | timeout 3 nc 127.0.0.1 "$port" > "$tmp/413"
check "1MB 초과 본문 → 413" grep -q '413' "$tmp/413"

printf 'POST /cgi-bin/postecho HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n' \
  | timeout 3 nc 127.0.0.1 "$port" > "$tmp/dupcl"
check "중복 Content-Length → 400" grep -q '400' "$tmp/dupcl"

printf 'POST /cgi-bin/postecho HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n' \
  | timeout 3 nc 127.0.0.1 "$port" > "$tmp/badcl"
check "숫자가 아닌 Content-Length → 400" grep -q '400' "$tmp/badcl"

printf 'POST /cgi-bin/postecho HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n' \
  | timeout 3 nc 127.0.0.1 "$port" > "$tmp/ovfcl"
check "Content-Length 오버플로 → 400" grep -q '400' "$tmp/ovfcl"

python3 - "$port" > "$tmp/uri414" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
s.sendall(b"GET /" + b"a" * 3000 + b" HTTP/1.1\r\n")
data = b""
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    data += chunk
print(data.decode(errors="replace"))
PY
check "URI 2KB 초과 → 414" grep -q '414' "$tmp/uri414"

python3 - "$port" > "$tmp/hdr431" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
request = b"GET / HTTP/1.1\r\nHost: x\r\n"
request += b"".join(b"X-%d: y\r\n" % i for i in range(99))
s.sendall(request)
data = b""
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    data += chunk
print(data.decode(errors="replace"))
PY
check "헤더 100줄 초과 → 431" grep -q '431' "$tmp/hdr431"

echo "== SIGCHLD, SIGPIPE/EPIPE =="
for _ in 1 2 3; do
  curl -s -o /dev/null "$url/cgi-bin/adder?1&2"
done
sleep 0.5
zombies=$(ps -o stat= --ppid "$pid" 2>/dev/null | grep -c 'Z' || true)
check "CGI 자식이 좀비로 남지 않음 (SIGCHLD)" test "${zombies:-0}" = 0

# SA_RESTART가 켜져 있어 EINTR 분기가 아닌 SIGCHLD 수신 시 생존만 확인한다.
kill -CHLD "$pid"
sleep 0.2
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "$url/")
check "accept 대기 중 SIGCHLD 수신 뒤에도 서버가 살아 있음 (SA_RESTART)" kill -0 "$pid"
check "accept 대기 중 SIGCHLD 수신 뒤에도 요청에 응답함 (SA_RESTART)" test "$code" = 200

python3 - "$port" <<'PY'
import socket, sys
p = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", p))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
s.close()  # 응답을 받기 전에 닫아 SIGPIPE/EPIPE를 만든다
PY
sleep 0.3
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "$url/")
check "조기 종료 뒤에도 서버가 살아 응답함" test "$code" = 200
check "조기 종료 뒤 서버 프로세스가 살아 있음" kill -0 "$pid"

python3 - "$port" <<'PY'
import socket, sys
p = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", p))
s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")  # 헤더를 끝내지 않고 닫는다
s.close()
PY
sleep 0.3
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "$url/")
check "헤더 중간 EOF 뒤에도 서버가 응답함" test "$code" = 200

echo
if [ "$fail" -eq 0 ]; then
  echo "ALL PASS"
else
  echo "SOME TESTS FAILED"
fi
exit "$fail"
