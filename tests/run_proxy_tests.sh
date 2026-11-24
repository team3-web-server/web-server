#!/usr/bin/env bash
# 프록시 테스트 스크립트: make test / make asan이 호출한다.
# 사용법: bash tests/run_proxy_tests.sh <프록시 실행 파일> <서버 실행 파일> <단위 테스트 실행 파일>
#   1) 백엔드로 웹 서버 세 개(18001, 18002, 18003)를 띄운다. 폴더마다 내용이 달라서 어느 백엔드가 답했는지 알 수 있다.
#   2) 프록시를 18100으로 띄우고 curl로 요청해, 경로에 맞는 백엔드의 응답이 돌아오는지 검사한다.
#   3) 단위 테스트를 돌린다.
set -u

proxy="${1:-build/proxy}"
server="${2:-build/server}"
unit="${3:-build/test_proxy}"
fail=0
pport=18100

pass() { echo "PASS: $proxy $1"; }
bad()  { echo "FAIL: $proxy $1"; fail=1; }

tmp=$(mktemp -d)
for n in 1 2 3; do
  mkdir -p "$tmp/b$n/api" "$tmp/b$n/static" "$tmp/b$n/cgi-bin"
  printf 'backend %s\n' "$n" > "$tmp/b$n/api/who.txt"
  printf 'backend %s\n' "$n" > "$tmp/b$n/static/who.txt"
  cp www/cgi-bin/echo "$tmp/b$n/cgi-bin/echo"
done
# 2초 뒤에 응답하는 CGI 프로그램. 느린 요청이 다른 요청을 기다리게 하는지 볼 때 쓴다
printf '#!/bin/sh\nsleep 2\nprintf "Content-Type: text/plain\\r\\n\\r\\nslow\\n"\n' > "$tmp/b2/cgi-bin/slow"
chmod +x "$tmp/b2/cgi-bin/slow"
head -c 1048576 /dev/urandom > "$tmp/b2/static/big.bin"   # 1MiB. 응답을 여러 번에 나눠 옮기게 한다
head -c 100000 /dev/urandom | base64 -w 0 > "$tmp/body.txt"  # 요청 버퍼(8192바이트)보다 큰 POST 본문

cat > "$tmp/proxy.conf" <<CONF
/api/     127.0.0.1:18001 127.0.0.1:18003
/static/  127.0.0.1:18002
/cgi-bin/ 127.0.0.1:18002
/down/    127.0.0.1:18009
CONF

pids=""
for n in 1 2 3; do
  "$server" "1800$n" "$tmp/b$n" > "$tmp/b$n.log" 2>&1 &
  pids="$pids $!"
done
"$proxy" "$pport" "$tmp/proxy.conf" > "$tmp/proxy.log" 2>&1 &
ppid=$!

# 포트가 연결을 받을 때까지 최대 5초 기다린다
wait_port() {
  for _ in $(seq 1 50); do
    if { exec 3<>/dev/tcp/127.0.0.1/$1; } 2>/dev/null; then exec 3>&-; return 0; fi
    sleep 0.1
  done
  bad "127.0.0.1:$1 에 연결하지 못함"
  return 1
}
wait_port 18001; wait_port 18002; wait_port 18003; wait_port "$pport"

# get <URL 경로> : 프록시로 GET을 보내고 "상태 코드 본문"을 출력한다
get() { curl -s --noproxy '*' -w ' %{http_code}' "http://127.0.0.1:$pport$1" | tr -d '\n'; }

# expect <이름> <받은 것> <기대>
expect() { if [ "$2" = "$3" ]; then pass "$1"; else bad "$1: 기대 '$3', 받은 것 '$2'"; fi; }

echo "== 경로 라우팅과 라운드로빈 =="
expect "/api/ 첫 번째 요청은 백엔드 18001" "$(get /api/who.txt)" "backend 1 200"
expect "/api/ 두 번째 요청은 백엔드 18003" "$(get /api/who.txt)" "backend 3 200"
expect "/static/ 요청은 백엔드 18002" "$(get /static/who.txt)" "backend 2 200"
expect "/api/ 세 번째 요청은 다시 백엔드 18001 (사이에 /static/ 요청이 있어도 순번 유지)" "$(get /api/who.txt)" "backend 1 200"
expect "/api/ 네 번째 요청은 백엔드 18003" "$(get /api/who.txt)" "backend 3 200"

echo "== 응답을 그대로 전달 =="
curl -s --noproxy '*' -o "$tmp/got.bin" "http://127.0.0.1:$pport/static/big.bin"
if cmp -s "$tmp/got.bin" "$tmp/b2/static/big.bin"; then pass "1MiB 파일이 바이트 그대로 전달됨"; else bad "1MiB 파일이 원본과 다름"; fi
expect "백엔드의 404도 그대로 전달" "$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' "http://127.0.0.1:$pport/api/nope.txt")" "404"

echo "== POST 본문 전달 =="
got=$(curl -s --noproxy '*' -d 'msg=hello' "http://127.0.0.1:$pport/cgi-bin/echo" | tail -n 1)
expect "짧은 본문이 백엔드의 CGI 프로그램까지 전달됨" "$got" "msg=hello"
curl -s --noproxy '*' --data-binary "@$tmp/body.txt" "http://127.0.0.1:$pport/cgi-bin/echo" | tail -n +3 | head -c -1 > "$tmp/echoed.txt"
if cmp -s "$tmp/echoed.txt" "$tmp/body.txt"; then pass "요청 버퍼보다 큰 본문(133336바이트)이 그대로 전달됨"; else bad "큰 본문이 원본과 다름"; fi

echo "== Forward Proxy (요청 대상이 절대 URL) =="
# curl -x <프록시 주소> : 요청을 프록시로 보내고, 요청 줄에 절대 URL을 적는다 (GET http://127.0.0.1:18001/api/who.txt HTTP/1.1)
fget() { curl -s --noproxy '' -x "http://127.0.0.1:$pport" -w ' %{http_code}' "$1" | tr -d '\n'; }
expect "절대 URL의 서버(18001)로 전달" "$(fget http://127.0.0.1:18001/api/who.txt)" "backend 1 200"
expect "설정 파일에 없는 경로와 서버여도 전달 (18003의 /static/)" "$(fget http://127.0.0.1:18003/static/who.txt)" "backend 3 200"
expect "주소가 이름(localhost)이어도 전달" "$(fget http://localhost:18002/static/who.txt)" "backend 2 200"
got=$(curl -s --noproxy '' -x "http://127.0.0.1:$pport" -d 'msg=hello' "http://127.0.0.1:18002/cgi-bin/echo" | tail -n 1)
expect "POST 본문도 전달" "$got" "msg=hello"
expect "연결할 수 없는 서버면 502" "$(curl -s --noproxy '' -x "http://127.0.0.1:$pport" -o /dev/null -w '%{http_code}' http://127.0.0.1:18009/)" "502"
# 여기까지 /api/ 로 시작하는 Reverse 요청은 다섯 번(18001, 18003, 18001, 18003, 404였던 18001)이었다
expect "Forward 요청은 라운드로빈 순번을 바꾸지 않음 (/api/ 여섯 번째 요청은 18003)" "$(get /api/who.txt)" "backend 3 200"
if grep -q '\[Forward\] GET http://127.0.0.1:18001/api/who.txt → 127.0.0.1:18001' "$tmp/proxy.log"; then pass "Forward 요청임을 출력함"; else bad "프록시 출력에 [Forward] 줄이 없음"; fi

echo "== 오류 =="
expect "맞는 규칙이 없는 경로는 404" "$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' "http://127.0.0.1:$pport/other")" "404"
expect "백엔드에 연결할 수 없으면 502" "$(curl -s --noproxy '*' -o /dev/null -w '%{http_code}' "http://127.0.0.1:$pport/down/x")" "502"
expect "502 뒤에도 프록시는 계속 동작" "$(get /static/who.txt)" "backend 2 200"

echo "== 연결마다 스레드 =="
# 2초 걸리는 요청을 먼저 보내고, 그 요청이 끝나기 전에 빠른 요청을 보낸다. 빠른 요청은 기다리지 않아야 한다
curl -s --noproxy '*' -o "$tmp/slow.out" "http://127.0.0.1:$pport/cgi-bin/slow" &
slow_pid=$!
sleep 0.3
fast_time=$(curl -s --noproxy '*' -o /dev/null -w '%{time_total}' "http://127.0.0.1:$pport/static/who.txt")
# awk로 소수 비교: 1초보다 짧으면 종료 코드 0
if awk -v t="$fast_time" 'BEGIN { exit !(t < 1.0) }'; then pass "느린 요청(2초)이 처리되는 동안 빠른 요청이 기다리지 않음 (${fast_time}초)"; else bad "빠른 요청이 느린 요청을 기다림 (${fast_time}초)"; fi
wait "$slow_pid"
expect "느린 요청도 끝까지 응답을 받음" "$(cat "$tmp/slow.out")" "slow"

# 동시에 보낸 /api/ 요청 40개가 두 백엔드에 정확히 20개씩 가야 한다 (순번을 잠금 없이 읽고 쓰면 한쪽으로 쏠릴 수 있다)
rr_pids=""
for i in $(seq 1 40); do
  curl -s --noproxy '*' "http://127.0.0.1:$pport/api/who.txt" > "$tmp/rr.$i" &
  rr_pids="$rr_pids $!"
done
wait $rr_pids
to1=$(cat "$tmp"/rr.* | grep -c 'backend 1')
to3=$(cat "$tmp"/rr.* | grep -c 'backend 3')
expect "동시 요청 40개가 두 백엔드에 20개씩" "$to1 $to3" "20 20"
if kill -0 "$ppid" 2>/dev/null; then pass "동시 요청 뒤에도 프록시 프로세스가 실행 중"; else bad "프록시 프로세스가 도중에 종료됨"; fi

echo "== keep-alive (클라이언트와의 연결 하나에서 요청 여러 개) =="
# send_raw <요청 바이트(printf 형식)>: 프록시에 연결 하나로 그대로 보내고, 프록시가 연결을 닫을 때까지 받은 것을 $tmp/raw.out에 담는다
send_raw() {
  : > "$tmp/raw.out"
  if { exec 3<>/dev/tcp/127.0.0.1/$pport; } 2>/dev/null; then
    printf "$1" >&3 2>/dev/null
    cat <&3 > "$tmp/raw.out" 2>/dev/null
    exec 3>&-
  fi
}
count_raw() { grep -c "$1" "$tmp/raw.out"; }

if curl -sv --noproxy '*' -o /dev/null -o /dev/null "http://127.0.0.1:$pport/static/who.txt" "http://127.0.0.1:$pport/static/who.txt" 2>&1 | grep -qi 're-using existing'; then
  pass "curl이 두 번째 요청에 프록시와의 연결을 다시 씀"
else
  bad "curl이 두 번째 요청에 프록시와의 연결을 다시 쓰지 않음"
fi

send_raw 'GET /static/who.txt HTTP/1.1\r\nHost: t\r\n\r\nGET /static/who.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n'
expect "요청 두 개를 연결 하나에서 처리 (응답 2개)" "$(count_raw 'HTTP/1.1 200 OK')" "2"
expect "첫 응답은 Connection: keep-alive, 닫자는 요청의 응답은 Connection: close" "$(count_raw 'Connection: keep-alive') $(count_raw 'Connection: close')" "1 1"

send_raw 'GET /static/who.txt HTTP/1.0\r\n\r\nGET /static/who.txt HTTP/1.0\r\n\r\n'
expect "HTTP/1.0 요청은 응답 하나 뒤에 연결을 닫음" "$(count_raw 'HTTP/1.1 200 OK')" "1"

# POST 본문 바로 뒤에 다음 요청이 붙어서 온다. 본문(5바이트)까지만 첫 요청으로 서버에 보내야 한다
send_raw 'POST /cgi-bin/echo HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nhelloGET /static/who.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n'
if [ "$(count_raw 'HTTP/1.1 200 OK')" -eq 2 ] && grep -q '^hello$' "$tmp/raw.out" && grep -q 'backend 2' "$tmp/raw.out"; then
  pass "POST 본문 뒤에 붙어 온 다음 요청을 경계에 맞게 나눠 전달"
else
  bad "POST 본문 뒤에 붙어 온 다음 요청을 나눠 전달하지 못함"
fi

send_raw 'GET /other HTTP/1.1\r\nHost: t\r\n\r\nGET /static/who.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n'
expect "프록시의 에러 응답(404) 뒤에는 연결을 닫음" "$(count_raw '^HTTP/1.1 ')" "1"

# 유지된 연결의 요청도 요청마다 백엔드를 번갈아 고른다 (서버와의 연결은 요청마다 새로 맺는다)
send_raw 'GET /api/who.txt HTTP/1.1\r\nHost: t\r\n\r\nGET /api/who.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n'
expect "연결 하나로 보낸 /api/ 요청 두 개가 두 백엔드에 하나씩" "$(count_raw 'backend 1') $(count_raw 'backend 3')" "1 1"

start=$SECONDS
send_raw 'GET /static/who.txt HTTP/1.1\r\nHost: t\r\n\r\n'
waited=$((SECONDS - start))
if [ "$waited" -ge 4 ] && [ "$waited" -le 7 ]; then pass "다음 요청이 없으면 5초 뒤 연결을 닫음 (${waited}초)"; else bad "유휴 연결을 닫기까지 ${waited}초 (5초여야 함)"; fi

echo "== 프록시 출력 =="
if grep -q 'GET /api/who.txt → 127.0.0.1:18001' "$tmp/proxy.log" && grep -q 'GET /api/who.txt → 127.0.0.1:18003' "$tmp/proxy.log"; then
  pass "어느 백엔드로 보냈는지 출력함"
else
  bad "프록시 출력에 백엔드 주소가 없음"; cat "$tmp/proxy.log"
fi

kill $ppid $pids 2>/dev/null
wait $ppid $pids 2>/dev/null

echo "== 프록시 단위 테스트 =="
if ! "$unit"; then fail=1; fi

rm -rf "$tmp"
if [ "$fail" -ne 0 ]; then echo "프록시 테스트 실패"; exit 1; fi
echo "프록시 테스트 모두 통과"
