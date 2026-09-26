#!/bin/sh
# FMQ 队列实验：开一条 port 63 的流（零采样＝无声），然后按 Q/W/S/D 摆弄那三块 ashmem。
#   sh qexp.sh                 只看三个队列头部
#   W="0:1:4" Q=2 sh qexp.sh   往队列 2 的 +0 写 u32=1（试 writePos 布局），等 S 毫秒后三个队列各再看一遍
# 每次都要重启 HAL：port 63 会被上一轮留下的流占住，不重启第二次 open 必被拒。
set -u
M=/data/local/tmp/q.log
setprop ctl.restart vendor.audio-hal-aidl
sleep 6
H=$(pidof audiohalservice.qti); echo "底 fd=$(ls /proc/$H/fd | wc -l)"
env DBG=1 STREAM=1 GOT=1 AUTO=1 TRANSACT=1 \
  Q="${Q:-0}" ${D:+D=1} S="${S:-1000}" ${W:+W=${W}} \
  SVC=android.hardware.audio.core.IModule/default \
  IN="63;1;8;0;0;L:2048;B;B" timeout 25 /data/local/tmp/argsloop > "$M" 2>&1
grep -a 'PFD@\|队列抓到\|前 q\|后 q\|写了\|STREAM 命中\|VERDICT\|M2a\|reply=' "$M" | head -30
H=$(pidof audiohalservice.qti); echo "尾 fd=$(ls /proc/$H/fd | wc -l)"
