#!/bin/sh
# A 路收官实验：接管轮里自建 mix config + 扬声器设备 config + AudioPatch，
# 然后开 DEEP_BUFFER 流、start、投零字节，看 HAL 的 configure 还报不报
# "no connected devices on stream!!"，以及 Reply.hardware.frames 是否推进。
# 全程零载荷（静音），一帧真实数据都不发。
# 用法：sh connect.sh            新建 patch（id=-1）
#       PIDF=1 sh connect.sh     改成"更新框架那条 patch 1"
# 出声档（要用户点头才能用）：TONE=440 AMP=3.2e7 ROUNDS=60 sh connect.sh
set -u
M=/data/local/tmp/conn.log
PIDF="${PIDF:--1}"
setprop ctl.stop audioserver
sleep 3
echo "audioserver=$(getprop init.svc.audioserver)"
logcat -c
DBG=1 APC=1 APCPORT=2 LOAD=/data/local/tmp/mix2.bin LOAD2=/data/local/tmp/dev23.bin DEVPORT=23 \
  TONE="${TONE:-}" AMP="${AMP:-}" ROUNDS="${ROUNDS:-}" SLEEPMS="${SLEEPMS:-}" \
  PP=1 LOADP=/data/local/tmp/patch0.bin PAUTO=1 SENDP=1 POKE=0:"$PIDF" \
  GOT=1 AUTO=1 TRANSACT=1 STREAM=1 SESSION=1 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" timeout 35 /data/local/tmp/argsloop > "$M" 2>&1
grep -aE 'LOAD|PP|SESSION|STREAM|TRANSACT st' "$M" | head -40
echo "== patch verdict"
logcat -d 2>/dev/null | grep -aiE 'setAudioPatch|connected devices|updateStreamsConnected' | tail -12
echo "== HAL/PAL"
logcat -d -s AHAL_Module_QTI AHAL_Stream_QTI AHAL_StreamOut_QTI AHAL_StreamOut_MI PAL AGM 2>/dev/null | tail -24
setprop ctl.start audioserver
sleep 3
echo "audioserver 恢复=$(getprop init.svc.audioserver)"
