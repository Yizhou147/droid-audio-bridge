#!/bin/sh
# 纯消费者视角：开流后只"读命令队列 + 放行"，不发我们的命令也不喂数据。
# 判据 = PULL 打出来的 tag/payload，以及 HAL write_db 的 CPU 是否开始涨。
set -u
M=/data/local/tmp/pull.log
setprop ctl.stop audioserver
sleep 3
echo "audioserver=$(getprop init.svc.audioserver)"
logcat -c
DBG=1 APC=1 APCPORT=2 LOAD=/data/local/tmp/cfg2.bin GOT=1 AUTO=1 TRANSACT=1 STREAM=1 \
  PULL=1 THR=1 SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" timeout 30 /data/local/tmp/argsloop > "$M" 2>&1
grep -a -E 'PULL|THR |LOAD:|reply=' "$M" | head -20
echo "== HAL"
logcat -d -s AHAL_Module_QTI AHAL_Stream_QTI AHAL_StreamOut_QTI AHAL_StreamOut_MI 2>/dev/null | tail -8
setprop ctl.start audioserver
sleep 3
echo "audioserver 恢复=$(getprop init.svc.audioserver)"
