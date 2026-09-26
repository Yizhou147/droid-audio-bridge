#!/bin/sh
# 安全诊断：直连 HAL 的 **low_latency** 流 + argsloop 内部小幅度正弦（不经实时音频、有界自动停）。
# 用法：TONECH=L|R|B sh lowl-tone.sh    （默认 B=双声道都给）
# 出声！跑之前务必先跟用户确认。AMP 默认满幅~1.5%（轻）。
set -u
M=/data/local/tmp/lowl.log
TONECH="${TONECH:-B}"
AMP="${AMP:-3.2e7}"
ROUNDS="${ROUNDS:-18}"
setprop ctl.stop audioserver; sleep 3
logcat -c
APC=1 APCPORT=1 LOAD=/data/local/tmp/mix1_lowlatency.bin LOAD2=/data/local/tmp/dev23.bin DEVPORT=23 \
 PP=1 LOADP=/data/local/tmp/patch0.bin PAUTO=1 SENDP=1 POKE=0:0 \
 GOT=1 AUTO=1 TRANSACT=1 STREAM=1 SESSION=1 \
 TONE=440 TONECH="$TONECH" AMP="$AMP" ROUNDS="$ROUNDS" SLEEPMS=300 FRAME=4 \
 SVC=android.hardware.audio.core.IModule/default IN="55;1;8;0;0;L:2048;B;B" \
 timeout 25 /data/local/tmp/argsloop > "$M" 2>&1
echo "== 本轮 usecase =="; logcat -d 2>/dev/null | grep -aE "usecase: (LOW_LATENCY|DEEP_BUFFER)|stream is configured|no connected" | tail -4 | cut -c1-100
grep -aE "SESSION 轮|PATCH 结果|created" "$M" | cut -c1-90 | head -4
setprop ctl.start audioserver; sleep 2; echo "audioserver=$(getprop init.svc.audioserver)"
