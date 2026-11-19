#!/usr/bin/env bash
# =====================================================================
# 개발 환경 점검 스크립트 (WEEK13 웹 서버 과제)
#
# 하는 일
#   과제에 필요한 도구가 설치되어 있는지 확인한다.
#   있으면 버전을 한 줄씩 출력하고, 없으면 MISSING으로 표시한다.
#   MISSING이 하나라도 있으면 종료 코드 1로 끝난다.
#
# 왜 필요한가
#   - 과제는 빌드(gcc, make), 메모리 검사(valgrind, -fsanitize),
#     기능 테스트(curl, nc), 부하 테스트(ab, wrk, hey)를 모두 요구한다.
#     하나라도 빠지면 테스트 단계에서 막히므로 시작 전에 한 번에 확인한다.
#   - 팀원마다 컴퓨터가 달라서 "내 컴퓨터에서는 되는데" 문제가 생기기 쉽다.
#     같은 스크립트로 점검하면 빠진 도구와 버전 차이를 바로 비교할 수 있다.
#   - 새 컴퓨터를 셋팅한 뒤 환경이 다 갖춰졌는지 증명하는 용도로도 쓴다.
#
# 사용법 (레포 최상위 폴더에서)
#   bash scripts/check-env.sh
#
# 없는 도구 설치 (Ubuntu / WSL2)
#   sudo apt update && sudo apt install -y build-essential gdb valgrind strace \
#     manpages-dev manpages-posix-dev curl netcat-openbsd apache2-utils wrk hey git
#   gh는 GitHub 공식 문서 방식으로 설치: https://cli.github.com/
# =====================================================================
set -u

missing=0

# 패키지 버전 조회 (버전 옵션이 없는 도구용)
pkgver() { dpkg-query -W -f='${Version}\n' "$1" 2>/dev/null; }

# check <표시 이름> <실행 파일> <버전 출력 명령...>
check() {
  local name="$1" bin="$2"; shift 2
  if command -v "$bin" >/dev/null 2>&1; then
    printf '%-18s %s\n' "$name" "$("$@" 2>&1 | head -n 1)"
  else
    printf '%-18s MISSING\n' "$name"
    missing=$((missing + 1))
  fi
}

# check_man <표시 이름> <섹션> <페이지>
check_man() {
  local name="$1" path; shift
  path="$(man -w "$@" 2>/dev/null | head -n 1)"
  if [ -f "$path" ]; then
    printf '%-18s OK (%s)\n' "$name" "$path"
  else
    printf '%-18s MISSING\n' "$name"
    missing=$((missing + 1))
  fi
}

echo "== 빌드 / 디버깅 / 메모리 검사 =="
check gcc       gcc      gcc --version
check make      make     make --version
check gdb       gdb      gdb --version
check valgrind  valgrind valgrind --version
check strace    strace   strace -V

echo "== HTTP / 소켓 / 부하 테스트 =="
check curl      curl     curl --version
check nc        nc       pkgver netcat-openbsd
check ab        ab       ab -V
check wrk       wrk      wrk -v
check hey       hey      pkgver hey

echo "== 버전 관리 =="
check git       git      git --version
check gh        gh       gh --version

echo "== POSIX API 매뉴얼 =="
check_man "man 2 socket"      2 socket
check_man "man 2 accept"      2 accept
check_man "man 3posix socket" 3posix socket

echo "=="
if [ "$missing" -eq 0 ]; then
  echo "결과: MISSING 없음"
else
  echo "결과: MISSING ${missing}개"
  exit 1
fi
