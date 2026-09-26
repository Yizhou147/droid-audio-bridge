#!/bin/sh
# A 路的完整会话实验：开 DEEP_BUFFER 流 → start → 取 Reply → burst → 投零字节 → 看 hardware.frames。
# 全程零载荷（静音）。用法：sh session.sh [portId]
set -u
APCPORT="${1:-2}"
M=/data/local/tmp/sess.log
setprop ctl.stop audioserver
sleep 3
echo "audioserver=$(getprop init.svc.audioserver)"
logcat -c
DBG=1 APC=1 APCPORT="$APCPORT" LOAD=/data/local/tmp/cfg2.bin GOT=1 AUTO=1 TRANSACT=1 STREAM=1 \
  SESSION=1 SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" timeout 30 /data/local/tmp/argsloop > "$M" 2>&1
grep -a SESSION /data/local/tmp/sess.log
echo "== HAL/PAL"
logcat -d -s AHAL_Module_QTI AHAL_Stream_QTI AHAL_StreamOut_QTI AHAL_StreamOut_MI PAL AGM 2>/dev/null | tail -14
setprop ctl.start audioserver
sleep 3
echo "audioserver 恢复=$(getprop init.svc.audioserver)"
