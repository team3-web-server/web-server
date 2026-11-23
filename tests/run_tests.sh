#!/usr/bin/env bash
# 테스트 실행 스크립트: make test / make asan이 호출한다.
# 사용법: bash tests/run_tests.sh <서버 실행 파일> <단위 테스트 실행 파일>...
#   서버를 한 번만 띄우고, 그 서버에 연결을 여섯 번 차례로 건다.
#     연결 1) 요청을 두 조각으로 나눠 보냄 → 200 응답 98바이트
#     연결 2) 요청 대상 /img/red.png → 200 응답 98바이트 (서버가 첫 연결 뒤에도 살아 있는지)
#     연결 3) 아무것도 보내지 않고 닫음 → 응답 없음
#     연결 4) 버퍼(8192바이트)보다 긴 헤더를 보냄 → 431 응답
#     연결 5) 요청 대상 / → 200 응답 (앞의 두 연결을 겪고도 서버가 살아 있는지)
#     연결 6) 요청 대상이 5000바이트 → 파일 경로가 버퍼(4096바이트)에 들어가지 않아 414 응답
#   서버는 요청 대상으로 만든 파일 경로를 출력한다. 연결 1, 2, 5의 경로가 기대와 같은지도 검사한다.
#   그 뒤 서버가 아직 실행 중인지 확인하고 끝낸다(kill). 마지막으로 단위 테스트 실행 파일들을 돌린다.
set -u

bin="${1:-build/server}"
shift   # 첫 번째 인자(서버)를 빼면 남은 인자 "$@"는 단위 테스트 실행 파일들이다
fail=0

# /dev/tcp/주소/포트 는 bash가 제공하는 기능으로, 열면 그 주소로 TCP 연결(connect)을 건다.
# 연결에 성공하면 그 연결은 fd 3번으로 열려 있고, 함수는 0을 돌려준다.
# 서버가 listen을 시작하기 전일 수 있으므로 최대 5초(0.1초 * 50번) 동안 다시 시도한다.
connect_to_server() {
  for _ in $(seq 1 50); do
    if { exec 3<>/dev/tcp/127.0.0.1/8080; } 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "FAIL: 127.0.0.1:8080에 연결하지 못함 (서버가 listen하지 않거나 이미 종료됨)"
  fail=1
  return 1
}

# 기대하는 200 응답 98바이트를 파일로 만들어 둔다. 받은 응답과 cmp로 한 바이트씩 비교한다 (cmp -s: 같으면 0, 화면 출력 없음)
expected=$(mktemp)
printf 'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 15\r\nConnection: close\r\n\r\n<h1>Hello</h1>\n' > "$expected"
resp=$(mktemp)

# 받은 응답 파일 $resp가 기대한 98바이트와 같은지 검사한다. $1은 화면에 보여 줄 연결 이름
check_200() {
  if cmp -s "$resp" "$expected"; then
    echo "PASS: $bin $1: 응답이 기대한 98바이트와 같음 (200 OK, 헤더 세 줄, 빈 줄, <h1>Hello</h1>)"
  else
    echo "FAIL: $bin $1: 응답이 기대와 다름. 받은 응답:"
    od -c "$resp" | head -20
    fail=1
  fi
}

echo "== 서버를 한 번 띄우고 연결을 여섯 번 건다 ($bin) =="
# 서버를 백그라운드(&)로 띄운다. 서버의 출력은 나중에 검사할 수 있게 임시 파일 $out에 받아 둔다.
out=$(mktemp)
"$bin" > "$out" &
pid=$!   # 방금 띄운 서버의 프로세스 번호

# 연결 1: 요청을 헤더 끝 표시 "\r\n\r\n"의 한가운데에서 잘라 두 번에 나눠 보낸다.
if connect_to_server; then
  printf 'GET /index.html HTTP/1.1\r\nHost: localhost:8080\r\n' >&3
  sleep 0.2
  printf '\r\n' >&3
  cat <&3 > "$resp"   # 서버가 연결을 닫을 때까지 응답을 모두 읽는다
  exec 3>&-           # 테스트 스크립트 쪽 연결 fd(3번)를 닫는다
  check_200 "연결 1(두 조각 48 + 2바이트)"
fi

# 연결 2: 같은 서버 프로세스에 다시 연결한다. 반복이 없으면 서버가 이미 끝나서 연결에 실패한다.
if connect_to_server; then
  printf 'GET /img/red.png HTTP/1.1\r\nHost: localhost:8080\r\n\r\n' >&3
  cat <&3 > "$resp"
  exec 3>&-
  check_200 "연결 2(같은 서버에 두 번째 요청)"
fi

# 연결 3: 요청을 보내지 않고 바로 닫는다. 서버의 read는 0을 돌려받는다.
if connect_to_server; then
  exec 3>&-
fi

# 연결 4: 값이 'a' 9000개인 헤더 줄 하나를 보낸다. 요청 전체가 버퍼 8192바이트보다 길다.
# head -c 9000 /dev/zero 는 0 바이트 9000개를 내고, tr 이 그것을 글자 a로 바꾼다.
if connect_to_server; then
  long=$(head -c 9000 /dev/zero | tr '\0' 'a')
  printf 'GET / HTTP/1.1\r\nHost: localhost:8080\r\nX-Long: %s\r\n\r\n' "$long" >&3 2>/dev/null
  cat <&3 > "$resp" 2>/dev/null
  exec 3>&-
  # head -n 1: 첫 줄만. tr -d '\r': 줄 끝의 \r을 지운다
  status=$(head -n 1 "$resp" | tr -d '\r')
  if [ "$status" = "HTTP/1.1 431 Request Header Fields Too Large" ] && grep -q '^Content-Length: 45' "$resp"; then
    echo "PASS: $bin 연결 4(버퍼보다 긴 헤더): 431 응답을 받음"
  else
    echo "FAIL: $bin 연결 4: 받은 상태 줄: '$status'"
    fail=1
  fi
fi

# 연결 5: 응답 없는 연결(3)과 에러 응답 연결(4)을 겪은 뒤에도 서버가 다음 연결을 받는지 확인한다.
if connect_to_server; then
  printf 'GET / HTTP/1.1\r\nHost: localhost:8080\r\n\r\n' >&3
  cat <&3 > "$resp"
  exec 3>&-
  check_200 "연결 5(연결 3, 4 뒤의 요청)"
fi

# 연결 6: 요청 대상이 '/' 뒤에 'a' 4999개(모두 5000바이트)다. 요청 전체는 버퍼 8192바이트 안에 들어가서 431은 아니지만,
# "./www"(5바이트)를 앞에 붙인 파일 경로 5005바이트는 경로 버퍼 4096바이트에 들어가지 않는다.
if connect_to_server; then
  long=$(head -c 4999 /dev/zero | tr '\0' 'a')
  printf 'GET /%s HTTP/1.1\r\nHost: localhost:8080\r\n\r\n' "$long" >&3 2>/dev/null
  cat <&3 > "$resp" 2>/dev/null
  exec 3>&-
  status=$(head -n 1 "$resp" | tr -d '\r')
  if [ "$status" = "HTTP/1.1 414 URI Too Long" ] && grep -q '^Content-Length: 26' "$resp"; then
    echo "PASS: $bin 연결 6(경로 버퍼보다 긴 요청 대상): 414 응답을 받음"
  else
    echo "FAIL: $bin 연결 6: 받은 상태 줄: '$status'"
    fail=1
  fi
fi
sleep 0.2   # 서버가 마지막 연결을 닫고 출력을 끝낼 시간을 준다
# kill -0 은 신호를 보내지 않고 그 프로세스가 있는지만 확인한다 (있으면 0)
if kill -0 "$pid" 2>/dev/null; then
  echo "PASS: $bin 연결 여섯 개를 처리한 뒤에도 서버 프로세스가 실행 중"
else
  echo "FAIL: $bin 서버 프로세스가 도중에 종료됨"
  fail=1
fi
kill "$pid" 2>/dev/null       # 서버에 종료 신호(SIGTERM)를 보낸다
wait "$pid" 2>/dev/null       # 서버가 끝날 때까지 기다린다
cat "$out"

# 서버 출력 검사. grep -c 는 찾는 줄의 개수를 센다. grep -q 는 있으면 성공(0)이고 화면에는 아무것도 출력하지 않는다
accepted=$(grep -c '^연결 수락: ' "$out")
closed=$(grep -c '^연결을 닫았습니다 (conn fd = ' "$out")
if [ "$accepted" -eq 6 ] && [ "$closed" -eq 6 ]; then
  echo "PASS: $bin 연결 수락 6번, 연결 닫기 6번"
else
  echo "FAIL: $bin 연결 수락 ${accepted}번, 연결 닫기 ${closed}번 (둘 다 6이어야 함)"
  fail=1
fi
# 닫은 conn fd 번호의 종류를 센다. 매번 닫으므로 같은 번호가 다시 쓰여 한 종류여야 한다
kinds=$(grep '^연결을 닫았습니다 (conn fd = ' "$out" | sort -u | wc -l)
if [ "$kinds" -eq 1 ]; then
  echo "PASS: $bin 여섯 연결 모두 같은 conn fd 번호를 다시 씀 (연결마다 close했다는 뜻)"
else
  echo "FAIL: $bin conn fd 번호가 ${kinds}종류 (close하지 않은 연결이 있음)"
  fail=1
fi
if grep -q '^받은 바이트 수: 50$' "$out" && grep -q '^요청 대상: /index.html$' "$out" && grep -q '^요청 대상: /img/red.png$' "$out" && grep -q '^요청 대상: /$' "$out"; then
  echo "PASS: $bin 연결 1, 2, 5의 요청 대상을 각각 /index.html, /img/red.png, /로 출력"
else
  echo "FAIL: $bin 출력에 받은 바이트 수 50 또는 요청 대상 줄이 없음"
  fail=1
fi
# 요청 대상으로 만든 파일 경로. "/"로 끝나는 요청 대상(연결 5)에는 index.html이 붙고, 연결 1과 경로가 같아져 두 줄이 된다
index_paths=$(grep -c '^파일 경로: ./www/index.html$' "$out")
if [ "$index_paths" -eq 2 ] && grep -q '^파일 경로: ./www/img/red.png$' "$out"; then
  echo "PASS: $bin 파일 경로를 ./www/index.html(연결 1, 5), ./www/img/red.png(연결 2)로 출력"
else
  echo "FAIL: $bin 파일 경로 출력이 기대와 다름 (./www/index.html 줄 ${index_paths}개)"
  fail=1
fi
if grep -q '^파일 경로가 너무 깁니다 (버퍼 4096바이트)$' "$out" && grep -q '^응답을 보냈습니다 (119바이트)$' "$out"; then
  echo "PASS: $bin 연결 6에서 파일 경로가 너무 길다고 출력하고 414 응답 119바이트를 보냄"
else
  echo "FAIL: $bin 연결 6의 출력이 기대와 다름"
  fail=1
fi
sent200=$(grep -c '^응답을 보냈습니다 (98바이트)$' "$out")
if [ "$sent200" -eq 3 ] && grep -q '^헤더가 끝나기 전에 연결이 닫혔습니다$' "$out" && grep -q '^431 응답을 보냈습니다 (157바이트)$' "$out"; then
  echo "PASS: $bin 200 응답 3번, 응답 없이 닫음 1번, 431 응답 1번"
else
  echo "FAIL: $bin 200 응답 ${sent200}번 또는 연결 3, 4의 출력이 기대와 다름"
  fail=1
fi
rm -f "$out" "$resp" "$expected"

for testbin in "$@"; do
  echo "== 단위 테스트: $testbin =="
  if ! "$testbin"; then
    fail=1
  fi
done

exit "$fail"
