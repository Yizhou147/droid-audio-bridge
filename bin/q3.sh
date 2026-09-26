#!/bin/sh
# 队列 2（16384 那块）+0 写 u32=1（试 FMQ 的 writePos），等 1.5 秒后把三块头部再读一遍。
# 如果对侧计数器动了 ⇒ 布局猜对，且这条直开的流真的在被 HAL 消费。仍然零采样＝无声。
M=/data/local/tmp/q3.log
setprop ctl.restart vendor.audio-hal-aidl
sleep 6
H=$(pidof audiohalservice.qti); echo "底 fd=$(ls /proc/$H/fd | wc -l)"
DBG=1 STREAM=1 GOT=1 AUTO=1 TRANSACT=1 Q=2 W=0:1:4 S=1500 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="63;1;8;0;0;L:2048;B;B" timeout 25 /data/local/tmp/argsloop > "$M" 2>&1
grep -a 'q[0-9]' "$M"
H=$(pidof audiohalservice.qti); echo "尾 fd=$(ls /proc/$H/fd | wc -l)"
