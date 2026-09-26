#!/bin/sh
# 用法：sh q4.sh <队列下标> <偏移:值:宽度>     例：sh q4.sh 0 0:1:4
# 往指定 FMQ 的指定位置写一个数（试 writePos 布局），等 2 秒后把三块头部再读一遍。
# 只要任何一处出现"对侧计数器变了"，就同时证明了：FMQ 元数据布局 + HAL 在轮询这条流。
M=/data/local/tmp/q4.log
Q="$1"; W="$2"
setprop ctl.restart vendor.audio-hal-aidl
sleep 6
H=$(pidof audiohalservice.qti); echo "底 fd=$(ls /proc/$H/fd | wc -l)"
DBG=1 STREAM=1 GOT=1 AUTO=1 TRANSACT=1 Q="$Q" W="$W" S=2000 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="63;1;8;0;0;L:2048;B;B" timeout 25 /data/local/tmp/argsloop > "$M" 2>&1
grep -a 'q[0-9](fd\|q[0-9] ' "$M"
H=$(pidof audiohalservice.qti); echo "尾 fd=$(ls /proc/$H/fd | wc -l)"
