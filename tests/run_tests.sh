#!/usr/bin/env bash
# 테스트 실행 스크립트: make test / make asan이 호출한다.
# 사용법: bash tests/run_tests.sh <서버 실행 파일> <단위 테스트 실행 파일>...
#   1) 서버를 띄우고 HTTP 요청을 두 조각으로 나눠 보낸 뒤, 서버가 요청 줄을 세 칸으로 나눠 출력하고
#      고정 응답(200 OK, <h1>Hello</h1>)을 보내고 정상 종료(종료 코드 0)하는지 확인
#   2) 서버를 다시 띄우고 아무것도 보내지 않은 채 연결을 닫아도 서버가 정상 종료하는지 확인
#   3) 서버를 다시 띄우고 버퍼(8192바이트)보다 긴 헤더를 보내면 431 응답이 오는지 확인
#   4) 단위 테스트 실행 파일들을 차례로 돌려 모두 PASS인지 확인
set -u

bin="${1:-build/server}"
shift   # 첫 번째 인자(서버)를 빼면 남은 인자 "$@"는 단위 테스트 실행 파일들이다
fail=0

# 서버를 백그라운드(&)로 띄우고, listen을 시작할 때까지 최대 5초(0.1초 * 50번) 동안 연결을 시도한다.
# 서버의 출력은 나중에 검사할 수 있게 임시 파일 $out에 받아 둔다.
# /dev/tcp/주소/포트 는 bash가 제공하는 기능으로, 열면 그 주소로 TCP 연결(connect)을 건다.
# 연결에 성공하면 그 연결은 fd 3번으로 열려 있고, 함수는 0을 돌려준다.
start_server_and_connect() {
  out=$(mktemp)
  "$bin" > "$out" &
  pid=$!   # 방금 띄운 서버의 프로세스 번호
  for _ in $(seq 1 50); do
    if { exec 3<>/dev/tcp/127.0.0.1/8080; } 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "FAIL: 127.0.0.1:8080에 연결하지 못함 (서버가 listen하지 않음)"
  kill "$pid" 2>/dev/null
  fail=1
  return 1
}

echo "== 서버 실행 1: 요청을 두 조각으로 나눠 보냄 ($bin) =="
resp=$(mktemp)
if start_server_and_connect; then
  # 요청을 헤더 끝 표시 "\r\n\r\n"의 한가운데에서 잘라 두 번에 나눠 보낸다.
  # 서버가 read 한 번만으로 끝내면 앞 조각에는 "\r\n\r\n"이 없어서 응답을 보내지 못한다.
  printf 'GET /index.html HTTP/1.1\r\nHost: localhost:8080\r\n' >&3
  sleep 0.2
  printf '\r\n' >&3
  cat <&3 > "$resp"   # 서버가 연결을 닫을 때까지 응답을 모두 읽는다
fi
wait "$pid"   # 서버가 끝날 때까지 기다리고 종료 코드를 받는다
code=$?
exec 3>&-     # 우리 쪽 연결 fd(3번)를 닫는다
cat "$out"
if [ "$code" -eq 0 ]; then
  echo "PASS: $bin 요청 하나를 받고 종료 코드 0"
else
  echo "FAIL: $bin 종료 코드 $code (0이어야 함)"
  fail=1
fi
# grep -q: 찾는 줄이 있으면 성공(0), 없으면 실패. 화면에는 아무것도 출력하지 않는다
if grep -q '^받은 바이트 수: 50$' "$out" && grep -q '^메서드: GET$' "$out" && grep -q '^요청 대상: /index.html$' "$out" && grep -q '^HTTP 버전: HTTP/1.1$' "$out"; then
  echo "PASS: $bin 두 조각(48 + 2바이트)을 이어 받아 요청 줄을 GET, /index.html, HTTP/1.1로 나눠 출력"
else
  echo "FAIL: $bin 출력에 받은 바이트 수 50 또는 메서드·요청 대상·버전 줄이 없음"
  fail=1
fi
# 응답 검사. 기대하는 98바이트를 파일로 만들어 cmp로 한 바이트씩 비교한다 (cmp -s: 같으면 0, 화면 출력 없음)
expected=$(mktemp)
printf 'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 15\r\nConnection: close\r\n\r\n<h1>Hello</h1>\n' > "$expected"
if cmp -s "$resp" "$expected"; then
  echo "PASS: $bin 응답이 기대한 98바이트와 같음 (200 OK, 헤더 세 줄, 빈 줄, <h1>Hello</h1>)"
else
  echo "FAIL: $bin 응답이 기대와 다름. 받은 응답:"
  od -c "$resp" | head -20
  fail=1
fi
rm -f "$out" "$resp" "$expected"

echo "== 서버 실행 2: 아무것도 보내지 않고 연결을 닫음 ($bin) =="
if start_server_and_connect; then
  exec 3>&-   # 요청을 보내지 않고 바로 닫는다. 서버의 read는 0을 돌려받는다
fi
wait "$pid"
code=$?
cat "$out"
if [ "$code" -eq 0 ] && grep -q '^헤더가 끝나기 전에 연결이 닫혔습니다$' "$out" && ! grep -q '^응답을 보냈습니다' "$out"; then
  echo "PASS: $bin 헤더가 끝나기 전에 닫힌 연결에는 응답하지 않고 종료 코드 0"
else
  echo "FAIL: $bin 종료 코드 $code 또는 출력이 기대와 다름"
  fail=1
fi
rm -f "$out"

echo "== 서버 실행 3: 버퍼보다 긴 헤더를 보냄 ($bin) =="
resp=$(mktemp)
if start_server_and_connect; then
  # 값이 'a' 9000개인 헤더 줄 하나를 보낸다. 요청 전체가 버퍼 8192바이트보다 길다.
  # head -c 9000 /dev/zero 는 0 바이트 9000개를 내고, tr 이 그것을 글자 a로 바꾼다.
  long=$(head -c 9000 /dev/zero | tr '\0' 'a')
  printf 'GET / HTTP/1.1\r\nHost: localhost:8080\r\nX-Long: %s\r\n\r\n' "$long" >&3 2>/dev/null
  cat <&3 > "$resp" 2>/dev/null
fi
wait "$pid"
code=$?
exec 3>&-
cat "$out"
# head -n 1: 첫 줄만. tr -d '\r': 줄 끝의 \r을 지운다
status=$(head -n 1 "$resp" | tr -d '\r')
if [ "$code" -eq 0 ] && [ "$status" = "HTTP/1.1 431 Request Header Fields Too Large" ] && grep -q '^Content-Length: 45' "$resp"; then
  echo "PASS: $bin 버퍼보다 긴 헤더에는 431 응답을 보내고 종료 코드 0"
else
  echo "FAIL: $bin 종료 코드 $code, 받은 상태 줄: '$status'"
  fail=1
fi
rm -f "$out" "$resp"

for testbin in "$@"; do
  echo "== 단위 테스트: $testbin =="
  if ! "$testbin"; then
    fail=1
  fi
done

exit "$fail"
