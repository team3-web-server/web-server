#!/usr/bin/env bash
# 테스트 실행 스크립트: make test가 호출한다.
# 사용법: bash tests/run_tests.sh <서버 실행 파일 경로>
# 단계 0: 실행 파일이 정상 종료(종료 코드 0)하는지만 확인한다.
set -u

bin="${1:-build/server}"
fail=0

"$bin"
code=$?
if [ "$code" -eq 0 ]; then
  echo "PASS: $bin 종료 코드 0"
else
  echo "FAIL: $bin 종료 코드 $code (0이어야 함)"
  fail=1
fi

exit "$fail"
