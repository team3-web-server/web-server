#!/usr/bin/env bash
# 클라이언트 커널이 SYN을 다시 보낼(재전송) 때마다 한 줄씩 출력한다. Ctrl+C로 끝낸다.
# /proc/net/netstat 의 TCPSynRetrans(SYN 재전송 횟수)와 ListenDrops(듣기 소켓이 버린 SYN 수)를 0.1초마다 읽는다.
read_counter() {
  awk -v name="$1" '/^TcpExt:/ { if (!h) { for (i = 1; i <= NF; i++) if ($i == name) col = i; h = 1 } else print $col }' /proc/net/netstat
}
base=$(read_counter TCPSynRetrans)
last=$base
start=$(date +%s%N)   # 나노초 단위 현재 시각
echo "SYN 재전송을 지켜보는 중... (지금을 0초로 봄)"
while true; do
  now=$(read_counter TCPSynRetrans)
  if [ "$now" != "$last" ]; then
    ms=$(( ($(date +%s%N) - start) / 1000000 ))   # 지난 시간(밀리초)
    printf "재전송 %d번째: %d.%d초 (서버가 버린 SYN 누적: %s)\n" "$((now - base))" "$((ms / 1000))" "$(( (ms % 1000) / 100 ))" "$(read_counter ListenDrops)"
    last=$now
  fi
  sleep 0.1
done
