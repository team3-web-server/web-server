#!/usr/bin/env bash
# 테스트 실행 스크립트: make test / make asan이 호출한다.
# 사용법: bash tests/run_tests.sh <서버 실행 파일> <단위 테스트 실행 파일>...
#   1) 임시 폴더에 www/ 를 복사해 정적 파일 폴더로 쓰고, 서버를 포트 18090으로 한 번 띄운다.
#   2) curl과 직접 만든 요청으로 응답의 상태 코드, Content-Type, 본문을 검사한다.
#   3) 서버가 끝까지 살아 있는지 확인하고 끝낸다(kill). 서버 출력도 검사한다.
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
printf 'secret\n' > "$root/noperm.txt"
chmod 000 "$root/noperm.txt"   # 읽을 권한을 모두 없앤다

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

sleep 0.2   # 서버가 마지막 연결을 닫고 출력을 끝낼 시간을 준다
# kill -0 은 신호를 보내지 않고 그 프로세스가 있는지만 확인한다 (있으면 0)
if kill -0 "$pid" 2>/dev/null; then pass "모든 요청을 처리한 뒤에도 서버 프로세스가 실행 중"; else bad "서버 프로세스가 도중에 종료됨"; fi
kill "$pid" 2>/dev/null       # 서버에 종료 신호(SIGTERM)를 보낸다
wait "$pid" 2>/dev/null       # 서버가 끝날 때까지 기다린다

echo "== 서버 출력 검사 =="
accepted=$(grep -c '^연결 수락: ' "$out")
closed=$(grep -c '^연결을 닫았습니다 (conn fd = ' "$out")
if [ "$accepted" -gt 0 ] && [ "$accepted" -eq "$closed" ]; then
  pass "연결 수락 ${accepted}번, 연결 닫기 ${closed}번 (같음)"
else
  bad "연결 수락 ${accepted}번, 연결 닫기 ${closed}번 (같아야 함)"
fi
# 닫은 conn fd 번호의 종류를 센다. 매번 닫고 파일 fd도 닫으므로 같은 번호가 다시 쓰여 한 종류여야 한다
kinds=$(grep '^연결을 닫았습니다 (conn fd = ' "$out" | sort -u | wc -l)
if [ "$kinds" -eq 1 ]; then pass "모든 연결이 같은 conn fd 번호를 다시 씀 (연결과 파일을 매번 close했다는 뜻)"; else bad "conn fd 번호가 ${kinds}종류 (닫지 않은 fd가 있음)"; fi
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
# AddressSanitizer(make asan)가 메모리 오류를 찾으면 서버 출력에 ERROR: 줄을 남긴다
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
