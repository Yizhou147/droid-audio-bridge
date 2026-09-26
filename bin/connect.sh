#!/bin/sh
# A 路最后一步：给这条 DEEP_BUFFER 流接上扬声器设备。
# 判据 = HAL 不再打 "no connected devices on stream!!"，且 SESSION 行里 hw.frames 不再是 -1。
# 全程零载荷（静音），不发一帧真实数据。
# 用法：sh connect.sh            （用我们自建的设备端口 config 当 sink）
#       PSINK=54 sh connect.sh   （改用框架那份现成的扬声器 config）
set -u
M=/data/local/tmp/conn.log
setprop ctl.stop audioserver
sleep 3
echo "audioserver=$(getprop init.svc.audioserver)"
logcat -c
DBG=1 APC=1 APCPORT=2 LOAD=/data/local/tmp/mix2.bin LOAD2=/data/local/tmp/dev23.bin DEVPORT=23 \
  PATCH=1 PATCHVAR="${PATCHVAR:-0123}" GOT=1 AUTO=1 TRANSACT=1 STREAM=1 SESSION=1 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" timeout 35 /data/local/tmp/argsloop > "$M" 2>&1
grep -aE 'LOAD|PATCH|SESSION|STREAM|ACP|TRANSACT st' "$M" | head -34
echo "== patch 相关判决"
logcat -d 2>/dev/null | grep -aiE 'patch|connected devices' | tail -12
echo "== HAL/PAL"
logcat -d -s AHAL_Module_QTI AHAL_Stream_QTI AHAL_StreamOut_QTI AHAL_StreamOut_MI PAL AGM 2>/dev/null | tail -22
setprop ctl.start audioserver
sleep 3
echo "audioserver 恢复=$(getprop init.svc.audioserver)"
