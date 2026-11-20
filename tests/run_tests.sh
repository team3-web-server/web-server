#!/usr/bin/env bash
# 테스트 실행 스크립트: make test / make asan이 호출한다.
# 사용법: bash tests/run_tests.sh <서버 실행 파일> <단위 테스트 실행 파일> [포트]
#   1) 인자 없이 실행하면 사용법을 출력하고 종료 코드 1인지 확인
#   2) 서버를 포트에 띄우고 curl로 한 번 접속 → 서버가 연결을 받고 닫은 뒤 종료 코드 0인지 확인
#   3) 단위 테스트 실행 파일을 돌려 모두 PASS인지 확인
set -u

bin="${1:-build/server}"
testbin="${2:-build/test_net}"
port="${3:-18080}"
fail=0

echo "== 서버 인자 검사: $bin =="
if "$bin" 2>/dev/null; then
  echo "FAIL: 포트 없이 실행했는데 종료 코드 0 (1이어야 함)"
  fail=1
else
  echo "PASS: 포트 없이 실행하면 사용법 출력 후 실패"
fi

echo "== 연결 하나 받고 닫기: $bin $port =="
# 서버가 accept에서 영원히 기다리지 않게 5초 제한
timeout 5 "$bin" "$port" &
pid=$!
# 서버가 listen할 때까지 최대 약 2초 재시도. 연결은 되지만 응답이 없으므로 curl은 52(Empty reply)로 끝난다.
# 서버가 요청을 읽지 않고 닫으면 커널이 RST를 보낼 수 있어 56(Connection reset)도 정상으로 본다
curl_code=7
for _ in $(seq 20); do
  curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$port/"
  curl_code=$?
  [ "$curl_code" -ne 7 ] && break # 7 = 연결 거부(아직 listen 전)
  sleep 0.1
done
wait "$pid"
code=$?
if [ "$curl_code" -eq 52 ] || [ "$curl_code" -eq 56 ]; then
  echo "PASS: curl이 연결된 뒤 빈 응답으로 끝남 (서버가 연결을 받고 닫음)"
else
  echo "FAIL: curl 종료 코드 $curl_code (52 또는 56 = 연결된 뒤 서버가 닫음이어야 함)"
  fail=1
fi
if [ "$code" -eq 0 ]; then
  echo "PASS: $bin 종료 코드 0"
else
  echo "FAIL: $bin 종료 코드 $code (0이어야 함, 124면 5초 안에 연결을 못 받음)"
  fail=1
fi

echo "== 단위 테스트: $testbin =="
if ! "$testbin"; then
  fail=1
fi

exit "$fail"
