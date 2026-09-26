#!/bin/sh
# 安全诊断：直连 HAL 的 **low_latency** 流 + argsloop 内部小幅度正弦（不经实时音频、有界自动停）。
# 用法：TONECH=L|R|B sh lowl-tone.sh    （默认 B=双声道都给）
# 出声！跑之前务必先跟用户确认。AMP 默认满幅~1.5%（轻）。
set -u
M=/data/local/tmp/lowl.log
TONECH="${TONECH:-B}"
AMP="${AMP:-3.2e7}"
ROUNDS="${ROUNDS:-18}"
AS_ORIG=$(getprop init.svc.audioserver)        # 进轮时它本该是 stopped；只恢复"我们动之前的状态"
[ "$AS_ORIG" = running ] && { setprop ctl.stop audioserver; sleep 3; }
logcat -c
APC=1 APCPORT=1 LOAD=/data/local/tmp/mix1_lowlatency.bin LOAD2=/data/local/tmp/dev23.bin DEVPORT=23 \
 PP=1 LOADP=/data/local/tmp/patch0.bin PAUTO=1 SENDP=1 POKE=0:0 \
 GOT=1 AUTO=1 TRANSACT=1 STREAM=1 SESSION=1 \
 TONE=440 TONECH="$TONECH" AMP="$AMP" ROUNDS="$ROUNDS" SLEEPMS=300 FRAME=4 \
 SVC=android.hardware.audio.core.IModule/default IN="55;1;8;0;0;L:2048;B;B" \
 timeout 25 /data/local/tmp/argsloop > "$M" 2>&1
echo "== 本轮 usecase =="; logcat -d 2>/dev/null | grep -aE "usecase: (LOW_LATENCY|DEEP_BUFFER)|stream is configured|no connected" | tail -4 | cut -c1-100
grep -aE "SESSION 轮|PATCH 结果|created" "$M" | cut -c1-90 | head -4
# 红线（09-26 实锤）：接管轮里 audioserver 必须保持 stopped —— 框架一活就和我们的直连流
# 抢同一个 HAL 属主，日志里能看到它自己开 deep_buffer 流，诊断结论全被污染。
# 所以这里只把状态恢复成"跑之前"的样子，不无脑 start。
[ "$AS_ORIG" = running ] && { setprop ctl.start audioserver; sleep 2; }
echo "audioserver=$(getprop init.svc.audioserver)（进来前=$AS_ORIG）"
