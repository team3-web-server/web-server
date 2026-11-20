#!/usr/bin/env bash
# 테스트 실행 스크립트: make test / make asan이 호출한다.
# 사용법: bash tests/run_tests.sh <서버 실행 파일> <단위 테스트 실행 파일>
#   1) 서버를 띄우고 연결을 한 번 걸어, 서버가 그 연결을 받고 정상 종료(종료 코드 0)하는지 확인
#   2) 단위 테스트 실행 파일을 돌려 모두 PASS인지 확인
set -u

bin="${1:-build/server}"
testbin="${2:-build/test_net}"
fail=0

echo "== 서버 실행: $bin =="
# 서버는 accept에서 연결을 기다리며 멈추므로, 백그라운드(&)로 띄우고 여기서 직접 연결을 한 번 건다.
"$bin" &
pid=$!   # 방금 띄운 서버의 프로세스 번호

# 서버가 listen을 시작할 때까지 최대 5초(0.1초 * 50번) 동안 연결을 시도한다.
# /dev/tcp/주소/포트 는 bash가 제공하는 기능으로, 열면 그 주소로 TCP 연결(connect)을 건다.
connected=0
for _ in $(seq 1 50); do
  if (exec 3<>/dev/tcp/127.0.0.1/8080) 2>/dev/null; then
    connected=1
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
if [ "$code" -eq 0 ]; then
  echo "PASS: $bin 연결 하나를 받고 종료 코드 0"
else
  echo "FAIL: $bin 종료 코드 $code (0이어야 함)"
  fail=1
fi

echo "== 단위 테스트: $testbin =="
if ! "$testbin"; then
  fail=1
fi

exit "$fail"
