#!/bin/sh
# 纯观察：开一条 DEEP_BUFFER 流后，什么命令都不发、数据也不喂，
# 只在 5 秒里高频比对三块共享内存（谁在动它），并列出 HAL 各 write_* 线程的 futex 等待地址。
# 全程零采样 ⇒ 无声。
set -u
M=/data/local/tmp/watch.log
setprop ctl.stop audioserver
sleep 3
echo "audioserver=$(getprop init.svc.audioserver)"
logcat -c
DBG=1 APC=1 APCPORT=2 LOAD=/data/local/tmp/cfg2.bin GOT=1 AUTO=1 TRANSACT=1 \
  WATCH="${W:-5000}" TIDS=1 SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" timeout 30 /data/local/tmp/argsloop > "$M" 2>&1
grep -a -E 'LOAD:|WATCH|TIDS|THR |reply=' "$M" | head -30
echo "== HAL"
logcat -d -s AHAL_Module_QTI AHAL_Stream_QTI AHAL_StreamOut_QTI 2>/dev/null | tail -6
setprop ctl.start audioserver
sleep 3
echo "audioserver 恢复=$(getprop init.svc.audioserver)"
