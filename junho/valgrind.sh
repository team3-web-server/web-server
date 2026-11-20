#!/usr/bin/env bash
# junho/의 모든 프로그램을 valgrind(memcheck)로 돌려 메모리 오류·누수·fd 누수를 확인한다.
# 사용법: bash junho/valgrind.sh [시작 포트]   (기본 18600, 포트 4개를 연달아 쓴다)
# 결과: 항목마다 PASS/FAIL 한 줄, 실패가 하나라도 있으면 종료 코드 1. valgrind 로그는 임시 폴더에 남기고 경로를 출력한다.
set -u
cd "$(dirname "$0")"

port="${1:-18600}"
logs=$(mktemp -d)
fail=0
pids=()
trap 'kill "${pids[@]}" 2>/dev/null' EXIT

# valgrind.supp: 시스템 라이브러리 문제와 서버를 SIGTERM으로 끌 때 남는 듣기 소켓만 좁게 무시한다 (파일 안 설명 참고)
VG=(valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=99 --suppressions="$PWD/valgrind.supp")

pass() { echo "PASS: $1"; }
failed() { echo "FAIL: $1"; fail=1; }

# valgrind 로그 하나를 검사한다. 인자: 이름, 로그 파일, 끝날 때 열려 있어도 되는 fd 개수
check_log() {
  local name=$1 log=$2 fds=$3
  local sup; sup=$(grep -o 'suppressed: [0-9]* from' "$log" | tail -n 1 | grep -o '[0-9]*')
  grep -q "ERROR SUMMARY: 0 errors" "$log" && pass "$name: 메모리·fd 오류 0 (valgrind.supp로 무시한 것 ${sup:-0}개)" || failed "$name: 메모리·fd 오류 ($log)"
  # 누수 없음: 힙을 전부 해제했거나, definitely/indirectly/possibly lost가 모두 0
  # possibly lost 중 glibc 내부 할당은 suppressed로 빠지므로 남은 값이 0이어야 한다
  if grep -q "All heap blocks were freed" "$log" ||
     { grep -q "definitely lost: 0 bytes" "$log" && grep -q "indirectly lost: 0 bytes" "$log" && grep -q "possibly lost: 0 bytes" "$log"; }; then
    pass "$name: 메모리 누수 0"
  else
    failed "$name: 메모리 누수 ($log)"
  fi
  grep -q "FILE DESCRIPTORS: $fds open" "$log" && pass "$name: 끝날 때 열린 fd $fds개" ||
    failed "$name: 열린 fd 개수가 $fds가 아님: $(grep -o 'FILE DESCRIPTORS: [0-9]* open' "$log") ($log)"
}

# 서버가 응답할 때까지 최대 약 5초(valgrind는 느리다). 인자: 시도할 명령
wait_ready() {
  for _ in $(seq 50); do
    "$@" > /dev/null 2>&1 && return 0
    sleep 0.1
  done
  return 1
}

make -s > /dev/null && make -s -C tiny > /dev/null || { echo "FAIL: 빌드"; exit 1; }

# 1) 단계 1 서버: 연결 하나 받고 스스로 끝난다. 끝날 때 fd는 표준 입력·출력·에러 3개만 남아야 한다
"${VG[@]}" --log-file="$logs/server.log" ./build/server "$port" > /dev/null &
pid=$!
wait_ready curl -s -o /dev/null "http://127.0.0.1:$port/"
# curl은 52/56으로 끝나므로 wait_ready는 실패로 끝나도 상관없다. 서버가 끝났는지가 중요하다
wait "$pid"; code=$?
[ "$code" -eq 0 ] && pass "단계 1 서버: 종료 코드 0" || failed "단계 1 서버: 종료 코드 $code"
check_log "단계 1 서버" "$logs/server.log" 3

# 2) Tiny: 무한 루프라 요청을 다 보낸 뒤 SIGTERM으로 끈다.
#    그때 열려 있어도 되는 fd는 표준 3개 + 듣기 소켓 1개 = 4개. 요청마다 연 연결 fd·파일 fd는 닫혀 있어야 한다
#    --trace-children: fork한 자식과 execve한 adder도 각자 로그(%p = pid)를 남긴다
cd tiny
tport=$((port + 1))
"${VG[@]}" --trace-children=yes --log-file="$logs/tiny.%p.log" ./tiny "$tport" > /dev/null 2>&1 &
pids+=($!)
url="http://127.0.0.1:$tport"
wait_ready curl -sf -o /dev/null "$url/" || failed "Tiny가 포트 $tport에서 응답"
for path in / /godzilla.gif "/cgi-bin/adder?15000&213" /no-such-file.html /cgi-bin/adder.c; do
  curl -s -o /dev/null "$url$path"
done
curl -s -o /dev/null -X POST "$url/"
kill "${pids[0]}"; wait "${pids[0]}" 2>/dev/null
tiny_log=$(grep -l "Command: ./tiny" "$logs"/tiny.*.log)
check_log "Tiny" "$tiny_log" 4
adder_log=$(grep -l "Command: ./cgi-bin/adder" "$logs"/tiny.*.log)
# adder는 Tiny가 dup2로 표준 출력을 연결 fd로 바꿔 둔 자식이다. 물려받은 연결 fd(4)와 듣기 fd(3)가 열린 채 실행된다(책 코드 그대로)
[ -n "$adder_log" ] && grep -q "ERROR SUMMARY: 0 errors" "$adder_log" && pass "adder(CGI 자식): 메모리 오류 0" || failed "adder(CGI 자식): 로그 없음 또는 메모리 오류"

# 3) 에코 서버·클라이언트
eport=$((port + 2))
"${VG[@]}" --log-file="$logs/echoserveri.log" ./echoserveri "$eport" > /dev/null &
pids+=($!)
wait_ready bash -c "exec 3<>/dev/tcp/127.0.0.1/$eport" || failed "echoserveri가 포트 $eport에서 응답"
out=$(printf 'hello\nworld\n' | "${VG[@]}" --log-file="$logs/echoclient.log" ./echoclient localhost "$eport")
[ "$out" = $'hello\nworld' ] && pass "echoclient: 보낸 줄이 그대로 돌아옴" || failed "echoclient: 출력 '$out'"
check_log "echoclient" "$logs/echoclient.log" 3
kill "${pids[1]}"; wait "${pids[1]}" 2>/dev/null
check_log "echoserveri" "$logs/echoserveri.log" 4

# 4) hostinfo
"${VG[@]}" --log-file="$logs/hostinfo.log" ./hostinfo localhost > /dev/null
check_log "hostinfo" "$logs/hostinfo.log" 3

echo "valgrind 로그: $logs"
exit "$fail"
