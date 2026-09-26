#!/bin/sh
# 直开 DEEP_BUFFER 流之后，试"喂数据"这一侧：往 dataMQ 的元素区(+16)写零载荷，
# 并把生产者计数器设成字节数，再对事件字发 FUTEX_WAKE。等 N 毫秒后看三块队列头部
# 有没有被对侧推进，同时看 HAL/PAL 有没有 start/pal_stream 日志。
# 全程零采样 ⇒ 静音。
set -u
LOADF=/data/local/tmp/cfg2.bin
BW="${BW:-640}"          # 一次喂多少字节
CNT="${CNT:-0}"          # 生产者计数器偏移：0 还是 8
M=/data/local/tmp/data.log
setprop ctl.stop audioserver
sleep 3
echo "audioserver=$(getprop init.svc.audioserver)"
logcat -c
DBG=1 APC=1 APCPORT=2 LOAD="$LOADF" GOT=1 AUTO=1 TRANSACT=1 STREAM=1 \
  Q=2 W="$CNT:$BW:8" FK=1 S="${S:-3000}" \
  SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" timeout 30 /data/local/tmp/argsloop > "$M" 2>&1
grep -a -E 'LOAD:|PFD@|q[0-9]|写了|FK:' "$M" | head -22
echo "== HAL/PAL"
logcat -d -s AHAL_Module_QTI AHAL_Stream_QTI AHAL_StreamOut_QTI AHAL_StreamOut_MI PAL AGM 2>/dev/null | tail -14
echo "== 崩溃计数 $(logcat -d | grep -ac 'Fatal signal.*argsloop')"
setprop ctl.start audioserver
sleep 3
echo "audioserver 恢复=$(getprop init.svc.audioserver)"
