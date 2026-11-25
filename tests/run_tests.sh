#!/usr/bin/env bash
# 테스트 실행 스크립트: make test / make asan이 호출한다.
# 사용법: bash tests/run_tests.sh <서버 실행 파일> <단위 테스트 실행 파일>...
#   1) 임시 폴더에 www/ 를 복사해 정적 파일 폴더로 쓰고, 서버를 포트 18090으로 한 번 띄운다.
#   2) curl과 직접 만든 요청으로 응답의 상태 코드, Content-Type, 본문을 검사한다.
#   3) 동시 연결, 안전 종료(SIGTERM, SIGINT)를 검사하고, 서버 출력도 검사한다.
#   4) 단위 테스트 실행 파일들을 돌린다.
set -u

bin="${1:-build/server}"
shift   # 첫 번째 인자(서버)를 빼면 남은 인자 "$@"는 단위 테스트 실행 파일들이다
fail=0
port=18090   # 개발 중에 8080으로 띄워 둔 서버, 단위 테스트(tests/test_net.c)가 쓰는 18080·18081과 겹치지 않는 포트

pass() { echo "PASS: $bin $1"; }
bad()  { echo "FAIL: $bin $1"; fail=1; }

# 임시 정적 파일 폴더: www/ 를 복사하고 테스트용 파일을 더한다
root=$(mktemp -d)
cp -r www/. "$root"
# 1MiB(1048576바이트)짜리 파일. 버퍼(8192바이트)보다 커서 read/write 반복을 여러 번 돌게 한다
head -c 1048576 /dev/urandom > "$root/big.bin"
head -c 8388608 /dev/zero > "$root/huge.bin"   # 8MiB. 응답 도중 클라이언트가 끊는 상황을 만들 때 쓴다
printf 'secret\n' > "$root/noperm.txt"
chmod 000 "$root/noperm.txt"   # 읽을 권한을 모두 없앤다
# 실패하는 경우를 확인할 CGI 프로그램들. 셸 스크립트도 실행 권한이 있으면 execve로 실행된다 (첫 줄의 #!/bin/sh 덕분)
printf '#!/bin/sh\nexit 3\n' > "$root/cgi-bin/fail"                         # 0이 아닌 값으로 끝남
printf '#!/bin/sh\necho "no blank line"\n' > "$root/cgi-bin/noheader"       # 출력에 빈 줄이 없음
printf '#!/bin/sh\nprintf "X-A: 1\\n\\nplain\\n"\n' > "$root/cgi-bin/notype"   # Content-Type 없이 \n만 쓰는 출력
printf '#!/bin/sh\nprintf "Content-Type: text/plain\\r\\n\\r\\nignored stdin\\n"\n' > "$root/cgi-bin/noread"   # 표준 입력을 읽지 않음
chmod +x "$root/cgi-bin/fail" "$root/cgi-bin/noheader" "$root/cgi-bin/notype" "$root/cgi-bin/noread"
printf '#!/bin/sh\necho x\n' > "$root/cgi-bin/noexec"                        # 실행 권한 없음
chmod 644 "$root/cgi-bin/noexec"

out=$(mktemp)    # 서버 출력
body=$(mktemp)   # 받은 응답 본문
resp=$(mktemp)   # 직접 만든 요청으로 받은 응답 전체

echo "== 서버를 띄운다: $bin $port $root =="
"$bin" "$port" "$root" > "$out" 2>&1 &
pid=$!

# /dev/tcp/주소/포트 는 bash가 제공하는 기능으로, 열면 그 주소로 TCP 연결(connect)을 건다 (fd 3번).
# 서버가 listen을 시작하기 전일 수 있으므로 최대 5초(0.1초 * 50번) 동안 다시 시도한다.
connect_to_server() {
  for _ in $(seq 1 50); do
    if { exec 3<>/dev/tcp/127.0.0.1/$port; } 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  bad "127.0.0.1:$port에 연결하지 못함 (서버가 listen하지 않거나 이미 종료됨)"
  return 1
}
# 서버가 준비될 때까지 기다린다 (연결했다가 아무것도 보내지 않고 닫는다)
if connect_to_server; then exec 3>&-; fi

# check_get <이름> <URL 경로> <기대 상태 코드> <기대 Content-Type> [내용을 비교할 파일]
# curl 옵션: -s 진행 표시 끔, -o 본문을 파일로, -w 끝난 뒤 상태 코드와 Content-Type 출력,
#            --noproxy '*' 프록시를 거치지 않음, --path-as-is 경로의 ../ 를 curl이 정리하지 않고 그대로 보냄
check_get() {
  local got
  got=$(curl -s --noproxy '*' --path-as-is -o "$body" -w '%{http_code} %{content_type}' "http://127.0.0.1:$port$2")
  if [ "$got" != "$3 $4" ]; then
    bad "$1: 기대 '$3 $4', 받은 것 '$got'"
  elif [ $# -ge 5 ] && ! cmp -s "$body" "$5"; then
    bad "$1: 받은 본문이 파일 $5 와 다름"
  else
    pass "$1: $3 $4"
  fi
}

# check_raw <이름> <요청 바이트(printf 형식)> <기대 상태 줄>
# curl로는 만들 수 없는 요청(틀린 형식, 없는 버전)을 직접 보내고 응답의 첫 줄을 검사한다.
check_raw() {
  if connect_to_server; then
    printf "$2" >&3 2>/dev/null
    cat <&3 > "$resp" 2>/dev/null   # 서버가 연결을 닫을 때까지 응답을 모두 읽는다
    exec 3>&-
    local status
    status=$(head -n 1 "$resp" | tr -d '\r')
    if [ "$status" = "$3" ]; then pass "$1: $3"; else bad "$1: 기대 '$3', 받은 것 '$status'"; fi
  fi
}

echo "== 정적 파일과 MIME 타입 =="
check_get "GET /" / 200 "text/html; charset=utf-8" "$root/index.html"
check_get "GET /index.html" /index.html 200 "text/html; charset=utf-8" "$root/index.html"
check_get "GET /style.css" /style.css 200 "text/css; charset=utf-8" "$root/style.css"
check_get "GET /app.js" /app.js 200 "text/javascript; charset=utf-8" "$root/app.js"
check_get "GET /img/red.png" /img/red.png 200 "image/png" "$root/img/red.png"
check_get "GET /img/blue.jpg" /img/blue.jpg 200 "image/jpeg" "$root/img/blue.jpg"
check_get "GET /big.bin (1MiB, 반복 read/write)" /big.bin 200 "application/octet-stream" "$root/big.bin"
check_get "쿼리 문자열은 떼고 파일을 찾음" "/index.html?a=1" 200 "text/html; charset=utf-8" "$root/index.html"

echo "== 오류 응답 =="
err="text/html; charset=utf-8"
check_get "없는 파일" /nope.html 404 "$err"
check_get "폴더를 요청 (끝에 / 없음)" /img 403 "$err"
check_get "index.html이 없는 폴더" /img/ 404 "$err"
check_get "../ 로 폴더 밖을 요청" /../Makefile 403 "$err"
check_get "경로 중간의 ../" /img/../index.html 403 "$err"
# id -u 는 사용자 번호를 출력한다. 0(root)은 권한 검사를 받지 않아 파일이 열리므로 이 검사를 건너뛴다
if [ "$(id -u)" -ne 0 ]; then
  check_get "읽을 권한이 없는 파일" /noperm.txt 403 "$err"
else
  echo "SKIP: 읽을 권한이 없는 파일 검사 (root로 실행 중)"
fi
long=$(head -c 4999 /dev/zero | tr '\0' 'a')   # 글자 a 4999개
check_get "경로 버퍼(4096)보다 긴 요청 대상" "/$long" 414 "$err"

check_raw "POST는 정적 파일에 허용하지 않음" 'POST /index.html HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n' "HTTP/1.1 405 Method Not Allowed"
if grep -q '^Allow: GET' "$resp"; then pass "405 응답에 Allow: GET 헤더가 있음"; else bad "405 응답에 Allow 헤더가 없음"; fi
check_raw "구현하지 않은 메서드" 'BREW / HTTP/1.1\r\nHost: t\r\n\r\n' "HTTP/1.1 501 Not Implemented"
check_raw "지원하지 않는 HTTP 버전" 'GET / HTTP/2.0\r\nHost: t\r\n\r\n' "HTTP/1.1 505 HTTP Version Not Supported"
check_raw "HTTP/1.0 요청은 받음" 'GET / HTTP/1.0\r\n\r\n' "HTTP/1.1 200 OK"
check_raw "요청 줄이 두 칸뿐" 'GET /\r\nHost: t\r\n\r\n' "HTTP/1.1 400 Bad Request"
check_raw "요청 대상이 /로 시작하지 않음" 'GET index.html HTTP/1.1\r\nHost: t\r\n\r\n' "HTTP/1.1 400 Bad Request"
longhdr=$(head -c 9000 /dev/zero | tr '\0' 'a')
check_raw "버퍼(8192)보다 긴 헤더" "GET / HTTP/1.1\r\nHost: t\r\nX-Long: $longhdr\r\n\r\n" "HTTP/1.1 431 Request Header Fields Too Large"

echo "== 헤더와 POST 본문 =="
check_raw "HTTP/1.1인데 Host 헤더가 없음" 'GET / HTTP/1.1\r\n\r\n' "HTTP/1.1 400 Bad Request"
check_raw "헤더 이름은 대소문자를 구분하지 않음 (hOsT)" 'GET / HTTP/1.1\r\nhOsT: t\r\n\r\n' "HTTP/1.1 200 OK"
check_raw "POST에 Content-Length가 없음" 'POST /index.html HTTP/1.1\r\nHost: t\r\n\r\n' "HTTP/1.1 411 Length Required"
check_raw "Content-Length가 숫자가 아님" 'POST /index.html HTTP/1.1\r\nHost: t\r\nContent-Length: abc\r\n\r\n' "HTTP/1.1 400 Bad Request"
check_raw "Content-Length가 1MiB를 넘음" 'POST /index.html HTTP/1.1\r\nHost: t\r\nContent-Length: 1048577\r\n\r\n' "HTTP/1.1 413 Content Too Large"
# curl -d '본문': POST 요청을 보내고 본문과 Content-Length 헤더를 붙여 준다
got=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -d 'name=somang' "http://127.0.0.1:$port/index.html")
if [ "$got" = "405" ]; then pass "curl -d로 보낸 POST (본문 11바이트): 405"; else bad "curl -d POST: 받은 코드 '$got'"; fi
# 본문을 헤더와 따로, 두 조각으로 나눠 보낸다. 서버는 Content-Length(11)만큼 모아야 한다
if connect_to_server; then
  printf 'POST /index.html HTTP/1.1\r\nHost: t\r\nContent-Length: 11\r\n\r\n' >&3
  sleep 0.2
  printf 'hello' >&3
  sleep 0.2
  printf ' world' >&3
  cat <&3 > "$resp"
  exec 3>&-
  if [ "$(head -n 1 "$resp" | tr -d '\r')" = "HTTP/1.1 405 Method Not Allowed" ]; then pass "본문을 두 조각으로 나눠 보낸 POST: 405"; else bad "본문을 나눠 보낸 POST"; fi
fi
# 본문 100000바이트 (헤더 버퍼 8192보다 큼): 파일로 만들어 curl --data-binary @파일 로 보낸다
head -c 100000 /dev/zero | tr '\0' 'x' > "$body"
got=$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' -H 'Expect:' --data-binary "@$body" "http://127.0.0.1:$port/index.html")
if [ "$got" = "405" ]; then pass "본문 100000바이트 POST: 405"; else bad "본문 100000바이트 POST: 받은 코드 '$got'"; fi
# Content-Length는 10인데 3바이트만 보내고 닫는다. 서버는 응답 없이 다음 연결로 넘어가야 한다
if connect_to_server; then
  printf 'POST /index.html HTTP/1.1\r\nHost: t\r\nContent-Length: 10\r\n\r\nabc' >&3
  exec 3>&-
fi

echo "== CGI (동적 요청) =="
plain="text/plain; charset=utf-8"
# check_body <이름> <찾을 글자>: 직전에 받은 본문($body)에 그 글자가 있는지 본다. grep -F 는 글자를 정규식이 아니라 그대로 찾는다
check_body() {
  if grep -qF -- "$2" "$body"; then pass "$1"; else bad "$1: 본문에 '$2'가 없음. 받은 본문:"; head -c 300 "$body"; fi
}
check_get "GET /cgi-bin/hello?name=somang" "/cgi-bin/hello?name=somang" 200 "$plain"
check_body "CGI가 쿼리 문자열을 환경 변수로 받음" "QUERY_STRING=name=somang"
check_body "CGI가 메서드를 환경 변수로 받음" "REQUEST_METHOD=GET"
cp "$body" "$resp"
check_get "GET /cgi-bin/hello?name=junho" "/cgi-bin/hello?name=junho" 200 "$plain"
check_body "쿼리 문자열이 바뀌면 응답도 바뀜" "QUERY_STRING=name=junho"
if cmp -s "$body" "$resp"; then bad "두 번의 CGI 응답이 같음 (요청마다 새로 만들어져야 함)"; else pass "두 번의 CGI 응답이 서로 다름 (요청마다 프로그램이 실행됨)"; fi
check_get "쿼리 문자열 없는 CGI 요청" /cgi-bin/hello 200 "$plain"
check_body "쿼리 문자열이 없으면 빈 값" "QUERY_STRING="

got=$(curl -s --noproxy '*' -o "$body" -w '%{http_code} %{content_type}' -d 'msg=hello' "http://127.0.0.1:$port/cgi-bin/echo")
if [ "$got" = "200 $plain" ]; then pass "POST /cgi-bin/echo: 200"; else bad "POST /cgi-bin/echo: 받은 것 '$got'"; fi
check_body "CGI가 본문 길이를 환경 변수로 받음" "CONTENT_LENGTH=9"
check_body "CGI가 표준 입력으로 본문을 받아 돌려줌" "msg=hello"
# 본문 100000바이트: 파이프 버퍼(64KiB)보다 큰 본문도 CGI까지 그대로 전달되는지 본다
head -c 100000 /dev/zero | tr '\0' 'y' > "$resp"
got=$(curl -s --noproxy '*' -o "$body" -w '%{http_code}' -H 'Expect:' --data-binary "@$resp" "http://127.0.0.1:$port/cgi-bin/echo")
if [ "$got" = "200" ] && [ "$(grep -c 'y' "$body")" -eq 1 ] && [ "$(tr -cd 'y' < "$body" | wc -c)" -eq 100000 ]; then
  pass "본문 100000바이트를 CGI가 그대로 돌려줌"
else
  bad "본문 100000바이트 CGI: 코드 '$got', y 개수 $(tr -cd 'y' < "$body" | wc -c)"
fi
check_get "GET으로 echo를 부르면 본문 0바이트" /cgi-bin/echo 200 "$plain"
check_body "본문이 없으면 CONTENT_LENGTH=0" "CONTENT_LENGTH=0"

check_get "Content-Type을 쓰지 않는 CGI는 text/plain" /cgi-bin/notype 200 "$plain"
check_body "줄 끝이 \\n뿐인 CGI 출력도 헤더와 본문을 나눔" "plain"
check_get "없는 CGI 프로그램" /cgi-bin/nope 404 "$err"
check_get "실행 권한이 없는 CGI 프로그램" /cgi-bin/noexec 403 "$err"
check_get "0이 아닌 값으로 끝나는 CGI 프로그램" /cgi-bin/fail 500 "$err"
check_get "출력에 빈 줄이 없는 CGI 프로그램" /cgi-bin/noheader 500 "$err"
got=$(curl -s --noproxy '*' -o "$body" -w '%{http_code}' -d 'unused=1' "http://127.0.0.1:$port/cgi-bin/noread")
if [ "$got" = "200" ]; then pass "표준 입력을 읽지 않는 CGI에 POST: 200 (서버가 끝나지 않음)"; else bad "표준 입력을 읽지 않는 CGI: '$got'"; fi
check_raw "CGI 경로에도 구현하지 않은 메서드는 501" 'DELETE /cgi-bin/hello HTTP/1.1\r\nHost: t\r\n\r\n' "HTTP/1.1 501 Not Implemented"
check_get "CGI 경로의 ../ 도 차단" /cgi-bin/../index.html 403 "$err"

echo "== 연결 다루기 =="
# 요청을 헤더 끝 표시 "\r\n\r\n"의 한가운데에서 잘라 두 번에 나눠 보낸다
if connect_to_server; then
  printf 'GET /index.html HTTP/1.1\r\nHost: t\r\n' >&3
  sleep 0.2
  printf '\r\n' >&3
  cat <&3 > "$resp"
  exec 3>&-
  if [ "$(head -n 1 "$resp" | tr -d '\r')" = "HTTP/1.1 200 OK" ]; then pass "두 조각으로 나눠 보낸 요청: 200"; else bad "두 조각으로 나눠 보낸 요청"; fi
fi
# 아무것도 보내지 않고 닫는다. 서버는 응답 없이 다음 연결로 넘어가야 한다
if connect_to_server; then exec 3>&-; fi
check_get "빈 연결 뒤에도 다음 요청을 받음" / 200 "text/html; charset=utf-8"
# 8MiB 파일을 요청하고 응답을 읽지 않은 채 바로 닫는다. 서버는 받는 쪽이 없는 연결에 write하게 된다 (SIGPIPE 상황)
if connect_to_server; then
  printf 'GET /huge.bin HTTP/1.1\r\nHost: t\r\n\r\n' >&3
  sleep 0.1
  exec 3>&-
fi
sleep 0.3
check_get "응답 도중 클라이언트가 끊은 뒤에도 다음 요청을 받음" / 200 "text/html; charset=utf-8"

echo "== 동시 연결 (연결마다 fork) =="
# 연결 A: 요청의 앞부분만 보내고 멈춰 둔다 (fd 3번을 연 채로 둔다)
if connect_to_server; then
  printf 'GET /index.html HTTP/1.1\r\nHost: t\r\n' >&3
  # 연결 B: A가 아직 끝나지 않았는데도 응답을 받아야 한다. --max-time 3: 3초 안에 못 받으면 실패
  got=$(curl -s --noproxy '*' --max-time 3 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/style.css")
  if [ "$got" = "200" ]; then pass "멈춰 있는 연결 A가 있어도 연결 B가 응답을 받음"; else bad "연결 A가 멈춘 동안 연결 B가 응답을 못 받음 (받은 코드 '$got')"; fi
  # 이제 A의 나머지를 보낸다. A도 정상 응답을 받아야 한다
  printf '\r\n' >&3
  cat <&3 > "$resp"
  exec 3>&-
  if [ "$(head -n 1 "$resp" | tr -d '\r')" = "HTTP/1.1 200 OK" ]; then pass "멈춰 있던 연결 A도 나머지를 보내자 200을 받음"; else bad "연결 A가 응답을 못 받음"; fi
fi
# 요청 30개를 동시에 보낸다. 명령 끝의 & 는 끝나기를 기다리지 않고 다음 명령으로 넘어가게 하고, wait 는 모두 끝날 때까지 기다린다
codes=$(mktemp)
for i in $(seq 1 30); do
  curl -s --noproxy '*' --max-time 10 -o /dev/null -w '%{http_code}\n' "http://127.0.0.1:$port/cgi-bin/hello?n=$i" >> "$codes" &
done
wait $(jobs -p | grep -v "^$pid$") 2>/dev/null
ok=$(grep -c '^200$' "$codes")
if [ "$ok" -eq 30 ]; then pass "동시에 보낸 요청 30개가 모두 200"; else bad "동시 요청 30개 중 200은 ${ok}개"; fi
rm -f "$codes"

sleep 0.5   # 자식 프로세스들이 연결을 닫고 끝날 시간을 준다
# pgrep -P <pid>: 그 프로세스의 자식 프로세스 번호들을 출력한다. SIGCHLD 핸들러가 모두 거뒀다면 남은 자식(좀비)이 없어야 한다
if [ -z "$(pgrep -P "$pid")" ]; then pass "끝난 자식 프로세스가 남아 있지 않음 (좀비 없음)"; else bad "서버의 자식 프로세스가 남아 있음: $(pgrep -P "$pid" | tr '\n' ' ')"; fi
# kill -0 은 신호를 보내지 않고 그 프로세스가 있는지만 확인한다 (있으면 0)
if kill -0 "$pid" 2>/dev/null; then pass "모든 요청을 처리한 뒤에도 서버 프로세스가 실행 중"; else bad "서버 프로세스가 도중에 종료됨"; fi

echo "== keep-alive (연결 하나에서 요청 여러 개) =="
# send_raw <요청 바이트(printf 형식)>: 연결 하나에 그대로 보내고, 서버가 연결을 닫을 때까지 받은 것을 $resp에 담는다
send_raw() {
  : > "$resp"
  if connect_to_server; then
    printf "$1" >&3 2>/dev/null
    cat <&3 > "$resp" 2>/dev/null
    exec 3>&-
  fi
}
# count_in_resp <글자열>: $resp에서 그 글자열이 있는 줄 수
count_in_resp() { grep -c "$1" "$resp"; }

send_raw 'GET /index.html HTTP/1.1\r\nHost: t\r\n\r\nGET /style.css HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n'
if [ "$(count_in_resp 'HTTP/1.1 200 OK')" -eq 2 ]; then pass "HTTP/1.1 요청 두 개를 연결 하나에서 처리 (응답 2개)"; else bad "연결 하나에 보낸 요청 두 개의 응답이 2개가 아님"; fi
if [ "$(count_in_resp 'Connection: keep-alive')" -eq 1 ] && [ "$(count_in_resp 'Connection: close')" -eq 1 ]; then
  pass "첫 응답은 Connection: keep-alive, Connection: close 요청의 응답은 Connection: close"
else
  bad "응답의 Connection 헤더가 기대와 다름"
fi

send_raw 'GET /index.html HTTP/1.0\r\n\r\nGET /style.css HTTP/1.0\r\n\r\n'
if [ "$(count_in_resp 'HTTP/1.1 200 OK')" -eq 1 ]; then pass "HTTP/1.0 요청은 응답 하나 뒤에 연결을 닫음"; else bad "HTTP/1.0 요청 뒤에 연결을 닫지 않음"; fi

send_raw 'GET /index.html HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /style.css HTTP/1.0\r\n\r\n'
if [ "$(count_in_resp 'HTTP/1.1 200 OK')" -eq 2 ]; then pass "HTTP/1.0 + Connection: keep-alive 는 연결을 유지"; else bad "HTTP/1.0 + Connection: keep-alive 인데 연결을 유지하지 않음"; fi

# POST 본문 바로 뒤에 다음 요청이 붙어서 온다. Content-Length(5)만큼만 본문으로 읽어야 다음 요청의 시작이 맞는다
send_raw 'POST /cgi-bin/echo HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nhelloGET /cgi-bin/hello?n=2 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n'
if [ "$(count_in_resp 'HTTP/1.1 200 OK')" -eq 2 ] && grep -q 'QUERY_STRING=n=2' "$resp"; then
  pass "POST 본문 뒤에 붙어 온 다음 요청을 경계에 맞게 읽음"
else
  bad "POST 본문 뒤에 붙어 온 다음 요청을 읽지 못함"
fi

send_raw 'GET /nope.html HTTP/1.1\r\nHost: t\r\n\r\nGET /index.html HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n'
if [ "$(count_in_resp '^HTTP/1.1 ')" -eq 1 ] && grep -q '404 Not Found' "$resp"; then pass "에러 응답(404) 뒤에는 연결을 닫음"; else bad "에러 응답 뒤에 연결을 닫지 않음"; fi

# curl은 URL을 두 개 주면 같은 서버에는 연결을 다시 쓴다. -v 출력의 "Re-using existing connection"으로 확인한다
# (curl 8.18.0은 "Reusing existing"이라고 출력한다. 두 표기를 모두 찾는다)
if curl -sv --noproxy '*' -o /dev/null -o /dev/null "http://127.0.0.1:$port/index.html" "http://127.0.0.1:$port/style.css" 2>&1 | grep -qiE 're-?using existing'; then
  pass "curl이 두 번째 요청에 연결을 다시 씀"
else
  bad "curl이 두 번째 요청에 연결을 다시 쓰지 않음"
fi

# 요청 하나를 보내고 가만히 있으면 서버가 5초 뒤에 연결을 닫아야 한다. SECONDS는 bash가 세는 경과 초
start=$SECONDS
send_raw 'GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n'
waited=$((SECONDS - start))
if [ "$waited" -ge 4 ] && [ "$waited" -le 7 ]; then pass "다음 요청이 없으면 5초 뒤 연결을 닫음 (${waited}초)"; else bad "유휴 연결을 닫기까지 ${waited}초 (5초여야 함)"; fi

echo "== 안전 종료 =="
# 멈춰 있는 연결을 하나 만든 뒤 SIGTERM을 보낸다. 서버는 새 연결을 받지 않고, 이 연결이 끝나기를 기다렸다가 끝나야 한다
if connect_to_server; then
  printf 'GET /index.html HTTP/1.1\r\nHost: t\r\n' >&3
  sleep 0.2
  kill "$pid" 2>/dev/null       # kill의 기본 시그널은 SIGTERM
  sleep 0.3
  if kill -0 "$pid" 2>/dev/null; then pass "SIGTERM을 받아도 처리 중인 연결이 있으면 기다림"; else bad "처리 중인 연결을 두고 서버가 끝남"; fi
  printf '\r\n' >&3
  cat <&3 > "$resp"
  exec 3>&-
  if [ "$(head -n 1 "$resp" | tr -d '\r')" = "HTTP/1.1 200 OK" ]; then pass "종료 요청 전에 받은 연결은 끝까지 응답함"; else bad "종료 중에 처리하던 연결이 응답을 못 받음"; fi
else
  kill "$pid" 2>/dev/null
fi
wait "$pid" 2>/dev/null       # 서버가 끝날 때까지 기다린다. wait의 종료 코드는 서버의 종료 코드다
code=$?
if [ "$code" -eq 0 ]; then pass "SIGTERM 뒤 서버가 종료 코드 0으로 끝남"; else bad "SIGTERM 뒤 서버의 종료 코드가 $code (0이어야 함)"; fi

# 서버를 다시 띄워 SIGINT(터미널의 Ctrl+C와 같은 시그널)로도 끝나는지 본다
out2=$(mktemp)
"$bin" "$port" "$root" > "$out2" 2>&1 &
pid2=$!
if connect_to_server; then exec 3>&-; fi
sleep 0.2
kill -INT "$pid2" 2>/dev/null
wait "$pid2" 2>/dev/null
code=$?
if [ "$code" -eq 0 ] && grep -q '^웹 서버 프로세스를 끝냅니다$' "$out2"; then pass "SIGINT 뒤 서버가 종료 코드 0으로 끝남"; else bad "SIGINT 뒤 종료 코드 $code 또는 종료 메시지 없음"; fi
if grep -q 'ERROR: ' "$out2"; then bad "두 번째 서버 출력에 ERROR가 있음:"; grep -A 12 'ERROR: ' "$out2"; fi
rm -f "$out2"

echo "== 서버 출력 검사 =="
accepted=$(grep -c '^연결 수락: ' "$out")
closed=$(grep -c '^연결을 닫았습니다 (자식 프로세스 ' "$out")
if [ "$accepted" -gt 0 ] && [ "$accepted" -eq "$closed" ]; then
  pass "연결 수락 ${accepted}번, 자식 프로세스가 연결을 닫은 것 ${closed}번 (같음)"
else
  bad "연결 수락 ${accepted}번, 자식 프로세스가 연결을 닫은 것 ${closed}번 (같아야 함)"
fi
# 부모 프로세스가 받은 conn fd 번호의 종류를 센다. 부모가 자기 몫을 매번 닫으면 같은 번호가 다시 쓰여 한 종류여야 한다
# sed 's/.*(conn fd = //' 는 줄에서 "(conn fd = " 까지를 지워 번호만 남긴다
kinds=$(grep '에게 연결을 맡겼습니다 (conn fd = ' "$out" | sed 's/.*(conn fd = //' | sort -u | wc -l)
if [ "$kinds" -eq 1 ]; then pass "부모 프로세스의 conn fd 번호가 한 종류 (부모가 연결마다 자기 몫을 close했다는 뜻)"; else bad "부모의 conn fd 번호가 ${kinds}종류 (닫지 않은 fd가 있음)"; fi
if grep -q "^파일 경로: $root/index.html$" "$out" && grep -q '^헤더가 끝나기 전에 연결이 닫혔습니다$' "$out"; then
  pass "파일 경로와 빈 연결 메시지를 출력"
else
  bad "서버 출력에 파일 경로 또는 빈 연결 메시지가 없음"
fi
if grep -q '^본문 11바이트: name=somang$' "$out" && grep -q '^본문 11바이트: hello world$' "$out" && grep -q '^본문 100000바이트: x' "$out"; then
  pass "서버가 받은 본문이 보낸 것과 같음 (name=somang, hello world, 100000바이트)"
else
  bad "서버 출력의 본문이 기대와 다름"
fi
if grep -q '^Host 헤더: t$' "$out" && grep -q '^본문을 다 받기 전에 연결이 끊겼습니다 (Content-Length 10)$' "$out"; then
  pass "Host 헤더 값과 본문 도중 끊긴 연결을 출력"
else
  bad "서버 출력에 Host 헤더 또는 끊긴 본문 메시지가 없음"
fi
if grep -q '^종료 요청을 받았습니다' "$out" && grep -q '^웹 서버 프로세스를 끝냅니다$' "$out"; then
  pass "종료 요청과 종료 메시지를 출력"
else
  bad "서버 출력에 종료 메시지가 없음"
fi
# AddressSanitizer(make asan)가 메모리 오류나 누수를 찾으면 서버 출력에 ERROR: 줄을 남긴다.
# 부모와 자식 프로세스 모두 정상 종료(return, exit)하므로 끝날 때 누수 검사가 실행된다
if grep -q 'ERROR: ' "$out"; then bad "서버 출력에 ERROR가 있음:"; grep -A 12 'ERROR: ' "$out"; fi

chmod 644 "$root/noperm.txt"
rm -rf "$root" "$out" "$body" "$resp"

for testbin in "$@"; do
  echo "== 단위 테스트: $testbin =="
  if ! "$testbin"; then
    fail=1
  fi
done

exit "$fail"
