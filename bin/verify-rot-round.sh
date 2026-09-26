#!/bin/bash
# 接管轮里验证 ROT=1 是否真的补齐四声道分配。
# 必须 setsid 脱离终端跑：进轮会把 kwin/pty 带走，本会话的交互式 shell 会死。
# 全程只读日志，唯一出声的地方是最后那次 16 秒、-22dBFS 的左右轮流裸流。
#
# 用法（容器里，跑在 desk-takeover 之前或之后都行，它会自己等）：
#   setsid nohup bash ~/Documents/XiaomiPad8Pro-drm-display/droid-audio-bridge/bin/verify-rot-round.sh \
#     >> ~/Documents/XiaomiPad8Pro-drm-display/logs/rot-round.log 2>&1 </dev/null &
set -u
BRIDGE=~/Documents/XiaomiPad8Pro-drm-display/droid-audio-bridge
LOGD=~/Documents/XiaomiPad8Pro-drm-display/logs
DEV=$(adb devices 2>/dev/null | awk '$2=="device"{print $1; exit}')
DEV=${DEV:-emulator-5554}
SINKPORT=${SINKPORT:-44777}
say() { echo "[$(date +%T)] $*"; }

say "== 等接管轮的 argsloop 起来（最多 180s）"
UP=0
for i in $(seq 1 90); do
    if adb -s "$DEV" shell "su -c 'pgrep -x argsloop'" 2>/dev/null | grep -qE '[0-9]'; then UP=1; break; fi
    sleep 2
done
[ "$UP" = 1 ] || { say "NO-ARGSLOOP：接管轮的 HAL sink 没起来，退出"; exit 1; }
say "argsloop 已起"

say "== 静默取证 1：我们这条 sink 自己的 ROT 行"
adb -s "$DEV" shell "su -c 'grep -aE \"ROT:|SESSION|SINK\" /data/local/tmp/hal-sink.log | tail -12'" 2>&1

sleep 5
say "== 静默取证 2：HAL/PAL 侧看到的 rotation（关键判据）"
adb -s "$DEV" shell "su -c 'logcat -d | grep -aE \"misound device rotation|Device Rotation Changed|Rotation for stream|SetOrientationCal\" | tail -8'" 2>&1

say "== 掐掉 aa-feeder（按 pid，不用 pkill -f 防自匹配），腾出 sink"
for p in $(pgrep -f "aa-feeder" 2>/dev/null); do
    [ "$p" = "$$" ] && continue
    kill "$p" 2>/dev/null && say "  killed $p"
done
for p in $(pgrep -f "pw-cat -r" 2>/dev/null); do
    [ "$p" = "$$" ] && continue
    kill "$p" 2>/dev/null && say "  killed pw-cat $p"
done
sleep 3

say "== 重新连 sink 并喂左右轮流裸流（-22dBFS，约 16s，之后自动停）"
python3 "$BRIDGE/bin/mkpan.py" --raw --amp 0.08 /tmp/pan-feed.raw || exit 1
say "  >>> 出声中：只左2s → 只右2s → 同相2s，两轮。请记『只右』那段在哪个位置"
nc -q1 127.0.0.1:"$SINKPORT" < /tmp/pan-feed.raw
say "  喂完"

sleep 2
say "== 收尾取证：sink 消费计数 + rotation 现状"
adb -s "$DEV" shell "su -c 'tail -6 /data/local/tmp/hal-sink.log; echo ---; logcat -d | grep -a \"misound device rotation\" | tail -3'" 2>&1
say "== 验证脚本结束"
