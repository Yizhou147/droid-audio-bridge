#!/bin/sh
# 安卓侧常驻 HAL sink（M3）：容器 aa-feeder 推来的 s16 PCM 经 argsloop 的 SINK 模式
# 直接喂进已跑通的 AIDL HAL 链，从板载喇叭外放——不依赖 audioserver（接管轮里它是死的）。
#
# 三个模板文件必须先在 /data/local/tmp（换机型要回 anland 重抓，见方案 §25/§31）：
#   mix2.bin   = deep_buffer_out(portId 2) 的 AudioPortConfig 线节（id 会在用时清 0 重建）
#   dev23.bin  = speaker(portId 23) 的设备端口 AudioPortConfig 线节
#   patch0.bin = 一条 AudioPatch 线节（源/汇 id 运行时由 PAUTO 改）
#
# 用法（root）：sh halsink.sh [port]        默认 44777
#   另开容器侧：  sh aa-feeder.sh 127.0.0.1:44777
#   出声测试：   AMP 无关（sink 用真信号），先用 pw-play /tmp/beep.wav 试探
#   收：         pkill -x argsloop          （或 kill 本脚本起的 pid）
set -u
PORT="${1:-44777}"
D=/data/local/tmp
for f in mix2.bin dev23.bin patch0.bin; do
  [ -e "$D/$f" ] || { echo "缺模板 $D/$f（先从 anland 跑 SAVE/SAVEP 抓，见方案 §25/§31）" >&2; exit 3; }
done
[ -x "$D/argsloop" ] || { echo "缺 $D/argsloop（CI 产物 push 上来）" >&2; exit 3; }
echo "HAL sink 起：SINK=$PORT（等容器 feeder 连；Ctrl-C 收）"
PIDF=0 SINK="$PORT" \
  APC=1 APCPORT=2 LOAD="$D/mix2.bin" LOAD2="$D/dev23.bin" DEVPORT=23 \
  PP=1 LOADP="$D/patch0.bin" PAUTO=1 SENDP=1 POKE=0:0 \
  AUTO=1 TRANSACT=1 STREAM=1 GOT=1 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" \
  exec "$D/argsloop"
