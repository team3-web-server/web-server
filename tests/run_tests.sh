#!/usr/bin/env bash
# 테스트 실행 스크립트: make test / make asan이 호출한다.
# 사용법: bash tests/run_tests.sh <서버 실행 파일> <단위 테스트 실행 파일>
#   1) 서버 실행 파일이 정상 종료(종료 코드 0)하는지 확인
#   2) 단위 테스트 실행 파일을 돌려 모두 PASS인지 확인
set -u

bin="${1:-build/server}"
testbin="${2:-build/test_net}"
fail=0

echo "== 서버 실행: $bin =="
"$bin"
code=$?
if [ "$code" -eq 0 ]; then
  echo "PASS: $bin 종료 코드 0"
else
  echo "FAIL: $bin 종료 코드 $code (0이어야 함)"
  fail=1
fi

echo "== 단위 테스트: $testbin =="
if ! "$testbin"; then
  fail=1
fi

exit "$fail"
