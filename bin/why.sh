#!/bin/sh
# 抓 openOutputStream 的失败原话：只看 AHAL 自己的 tag，避免别的进程日志插进来。
# 用法：sh why.sh [portConfigId 兜底值] [APCPORT]
set -u
APCPORT="${2:-2}"
M=/data/local/tmp/why.log
setprop ctl.restart vendor.audio-hal-aidl
sleep 8
logcat -c
DBG=1 APC=1 APCPORT="$APCPORT" GOT=1 AUTO=1 TRANSACT=1 STREAM=1 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" timeout 30 /data/local/tmp/argsloop > /data/local/tmp/why.args.log 2>&1
echo "== argsloop 关键行"
grep -a -E 'ACP3|portConfigId|reply=' /data/local/tmp/why.args.log | head -6
echo "== HAL 原话"
logcat -d -s AHAL_Module_QTI AHAL_Module AHAL_Stream_QTI AHAL_StreamOut_QTI AHAL_StreamOut_MI PAL 2>/dev/null | tail -24
echo "== 崩溃?"
logcat -d | grep -a -c 'Fatal signal.*argsloop'
