#!/usr/bin/env bash
# 데모 페이지(Web Server Lab)에 필요한 프로세스를 한 번에 띄운다.
#   백엔드 8001, 8003 (/api/), 8002 (/static/)  : build/server
#   백엔드 8004 (/cgi-bin/)                      : build/server (폴더 www: CGI 프로그램 hello, echo)
#   웹 서버 8005 (fork 없는 비교용 빌드)        : build/server-nofork
#   패킷 캡처 (있으면)                         : tcpdump (포트 8004의 패킷을 demo/lab/run/tcpdump.log 에 적는다)
#   프록시 8081 (스레드 없는 비교용 빌드)     : build/proxy-nothread
#   프록시 8080                                  : build/proxy (규칙은 demo/lab/proxy.conf, 출력은 demo/lab/run/proxy.log 에 쌓인다)
#   데모 페이지 서버 8000                        : build/server (페이지와 CGI 프로그램 lab 을 서빙)
# 실행: bash demo/lab/run.sh   →   브라우저에서 http://localhost:8000/
# 끝내기: Ctrl+C (띄운 프로세스를 모두 끝낸다)
set -u
cd "$(dirname "$0")/../.."

make lab || exit 1

run=demo/lab/run
mkdir -p "$run"
rm -f "$run"/*.pid "$run"/*.log

# 이 스크립트가 끝날 때, 프로세스 번호 파일에 적힌 프로세스를 모두 끝낸다 (페이지의 버튼으로 다시 켠 백엔드 포함)
cleanup() {
  for f in "$run"/*.pid; do
    [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null
  done
  rm -f "$run"/*.pid
}
trap cleanup EXIT
trap 'exit 0' INT TERM

# start 이름 명령...: 뒤에서 실행하고, 출력은 run/이름.log 에, 프로세스 번호는 run/이름.pid 에 적는다
start() {
  local name=$1
  shift
  "$@" > "$run/$name.log" 2>&1 &
  echo $! > "$run/$name.pid"
}

start 8001 ./build/server 8001 demo/proxy/backend1
start 8002 ./build/server 8002 demo/proxy/backend2
start 8003 ./build/server 8003 demo/proxy/backend3
start 8004 ./build/server 8004 www
start 8005 ./build/server-nofork 8005 www
start proxy ./build/proxy 8080 demo/lab/proxy.conf
start 8081 ./build/proxy-nothread 8081 demo/lab/proxy.conf
start lab ./build/server 8000 demo/lab/www

sleep 0.5
for name in 8001 8002 8003 8004 8005 proxy 8081 lab; do
  if ! kill -0 "$(cat "$run/$name.pid")" 2>/dev/null; then
    echo "실행 실패: $name ($run/$name.log 의 마지막 줄)"
    tail -n 3 "$run/$name.log"
    echo
    echo "\"Address already in use\"가 보이면 전에 띄운 서버가 남아 있는 것이다. 아래 명령으로 끝낸 뒤 다시 실행한다."
    echo "  pkill -f build/server; pkill -f build/proxy"
    exit 1
  fi
done

# 패킷 캡처: 포트 8004로 오가는 TCP 패킷을 한 줄씩 run/tcpdump.log 에 적는다 (TCP Handshake와 레이어 장면이 읽는다).
# -i lo: 이 컴퓨터 안에서 오가는 패킷, -l: 줄마다 바로 출력, -n: 주소를 숫자로, -tt: 시각을 숫자로, -S: seq, ack를 실제 값으로
# 패킷 캡처는 관리자 권한이 필요하다. 권한이 없으면 이 장면만 동작하지 않고 나머지는 그대로 동작한다.
if command -v tcpdump > /dev/null; then
  start tcpdump tcpdump -i lo -l -n -tt -S tcp port 8004
  sleep 0.5
  if ! kill -0 "$(cat "$run/tcpdump.pid")" 2>/dev/null; then
    rm -f "$run/tcpdump.pid"
    echo "패킷 캡처를 켜지 못했다 (TCP Handshake와 레이어 장면만 동작하지 않는다). 아래 명령을 한 번 실행한 뒤 다시 실행한다."
    echo '  sudo setcap cap_net_raw,cap_net_admin=eip "$(readlink -f "$(which tcpdump)")"'
  fi
else
  echo "tcpdump가 없다 (TCP Handshake와 레이어 장면만 동작하지 않는다). 설치: sudo apt install -y tcpdump"
fi

echo
echo "데모 페이지: http://localhost:8000/"
echo "프록시 로그: tail -f $run/proxy.log"
echo "끝내려면 Ctrl+C"
wait
