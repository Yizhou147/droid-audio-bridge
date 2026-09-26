#!/bin/sh
# A 路里程碑实验：直开一条输出流 → 往 CommandMQ 发命令（默认 start）→ 回读 ReplyMQ。
# 用法：sh fmq.sh [portConfigId] [命令 tag]   （tag: 1 getStatus / 2 start / 3 burst / 5 standby）
# 判据不看耳朵：Reply 各字段 + HAL 的 AHAL_StreamOut_QTI: start 日志。全程零采样。
set -u
PID="${1:-55}"; TAG="${2:-2}"
M=/data/local/tmp/fmq.log
if [ "${NORESTART:-0}" != 1 ]; then setprop ctl.restart vendor.audio-hal-aidl; sleep 8; fi
H=$(pidof audiohalservice.qti); echo "底 fd=$(ls /proc/$H/fd | wc -l)"
logcat -c
DBG=1 GOT=1 AUTO=1 TRANSACT=1 STREAM=1 CMD="$TAG:0" CW="${CW:-8}" S=2500 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="$PID;1;8;0;0;L:2048;B;B" timeout 25 /data/local/tmp/argsloop > "$M" 2>&1
grep -a 'PFD@\|CMD:\|reply=\|q[0-9]' "$M"
echo "--- HAL 侧"
logcat -d 2>/dev/null | grep -a "AHAL_" | grep -av getAudioPorts | head -8
H=$(pidof audiohalservice.qti); echo "尾 fd=$(ls /proc/$H/fd | wc -l)"
