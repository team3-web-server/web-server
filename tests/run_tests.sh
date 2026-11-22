#!/usr/bin/env bash
# 테스트 실행 스크립트: make test / make asan이 호출한다.
# 사용법: bash tests/run_tests.sh <서버 실행 파일> <단위 테스트 실행 파일>...
#   1) 서버를 띄우고 연결을 걸어 HTTP 요청을 보낸 뒤, 서버가 요청 줄을 세 칸으로 나눠 출력하고
#      정상 종료(종료 코드 0)하는지 확인
#   2) 단위 테스트 실행 파일들을 차례로 돌려 모두 PASS인지 확인
set -u

bin="${1:-build/server}"
shift   # 첫 번째 인자(서버)를 빼면 남은 인자 "$@"는 단위 테스트 실행 파일들이다
fail=0

echo "== 서버 실행: $bin =="
# 서버는 accept에서 연결을 기다리며 멈추므로, 백그라운드(&)로 띄우고 여기서 직접 연결을 한 번 건다.
# 서버의 출력은 나중에 검사할 수 있게 임시 파일에 받아 둔다.
out=$(mktemp)
"$bin" > "$out" &
pid=$!   # 방금 띄운 서버의 프로세스 번호

# 서버가 listen을 시작할 때까지 최대 5초(0.1초 * 50번) 동안 연결을 시도한다.
# /dev/tcp/주소/포트 는 bash가 제공하는 기능으로, 열면 그 주소로 TCP 연결(connect)을 건다.
# 서버는 연결을 하나만 받으므로, 연결에 성공한 그 fd(3번)로 바로 요청을 보낸다.
connected=0
for _ in $(seq 1 50); do
  if { exec 3<>/dev/tcp/127.0.0.1/8080; } 2>/dev/null; then
    connected=1
    printf 'GET /index.html HTTP/1.1\r\nHost: localhost:8080\r\n\r\n' >&3   # HTTP 요청을 보낸다
    break
  fi
  sleep 0.1
done
if [ "$connected" -eq 0 ]; then
  echo "FAIL: 127.0.0.1:8080에 연결하지 못함 (서버가 listen하지 않음)"
  kill "$pid" 2>/dev/null
  fail=1
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
if grep -q '^메서드: GET$' "$out" && grep -q '^요청 대상: /index.html$' "$out" && grep -q '^버전: HTTP/1.1$' "$out"; then
  echo "PASS: $bin 요청 줄을 GET, /index.html, HTTP/1.1 세 칸으로 나눠 출력"
else
  echo "FAIL: $bin 출력에 메서드·요청 대상·버전 줄이 없음"
  fail=1
fi
rm -f "$out"

for testbin in "$@"; do
  echo "== 단위 테스트: $testbin =="
  if ! "$testbin"; then
    fail=1
  fi
done

exit "$fail"
