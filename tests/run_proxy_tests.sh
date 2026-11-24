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
