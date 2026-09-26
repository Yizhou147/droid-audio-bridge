#!/bin/sh
# 接管轮验证：把"只左/只右轮流"的裸 PCM 喂进我们自己的直连 HAL sink（argsloop SINK 模式），
# 用来确认 ROT=1 之后四声道分配是否正确。
#
# 红线：直连链路没有任何音量控制，所以这里幅度只有 0.08（约 -22 dBFS），
#       而且整段 ~16 秒后自己停。要更响就显式传 PAN_AMP，别顺手喂真实音频。
#
# 用法（容器里，接管轮已起、halsink 在 44777 监听）：
#   sh bin/pan-feed.sh [host:port]
set -u
DST="${1:-127.0.0.1:44777}"
HOST="${DST%%:*}"; PORT="${DST##*:}"
RAW=/tmp/pan-feed.raw
PAN_AMP="${PAN_AMP:-0.08}" python3 "$(dirname "$0")/mkpan.py" --raw --amp "${PAN_AMP:-0.08}" "$RAW" || exit 1
echo "== 喂 $RAW 到 $HOST:$PORT（~16s：静音→只左→静音→只右→静音→左右同相，两轮）"
echo "   听到『只右』那段时，它应该在【右边两个喇叭】；若跑到下边/上边，ROT 没生效"
nc -q1 "$HOST" "$PORT" < "$RAW"
echo "== 喂完退出"
