#!/bin/bash
cd /root/work/soma-v3
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  [ "$pid" = "$$" ] && continue
  # 只认「argv[0] 是 node、argv[1 是 server.js」——按整串子串匹配会匹配到这条 bash 自己（踩过两次）
  a0=$(tr '\0' '\n' < "$p/cmdline" 2>/dev/null | sed -n 1p)
  a1=$(tr '\0' '\n' < "$p/cmdline" 2>/dev/null | sed -n 2p)
  [ "$(basename "$a0" 2>/dev/null)" = "node" ] || continue
  [ "$(basename "$a1" 2>/dev/null)" = "server.js" ] || continue
  kill "$pid" 2>/dev/null
done
sleep 1
# 容器的时钟是 UTC，手机是 +08 —— 她的作息（褪黑素、夜里犯困）必须跟人一样，
# 所以显式把时区给进去；C++ 的 localtime() 和 node 的 Date 都会听这个变量。
export TZ="${SOMA_TZ:-Asia/Shanghai}"
setsid nohup node server.js > /tmp/soma3d.log 2>&1 < /dev/null &
sleep 1.5
tail -1 /tmp/soma3d.log
