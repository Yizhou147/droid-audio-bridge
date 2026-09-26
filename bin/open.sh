#!/bin/sh
# 接管轮的真实场景就是 audioserver 被 stop 掉 ⇒ 框架在输出端口上的那条流也没了，
# 我们才是唯一的使用者。这里在 anland 下先手动 stop audioserver 模拟，跑完再 start 回去。
# 全程零采样（只 open + 发命令 + 读回包），不会出声。
APCPORT="${1:-2}"
setprop ctl.stop audioserver
sleep 3
echo "audioserver=$(getprop init.svc.audioserver)"
logcat -c
DBG=1 APC=1 APCPORT="$APCPORT" LOAD="$LOAD" GOT=1 AUTO=1 TRANSACT=1 STREAM=1 THR=1 \
  CMD=2:0 CW="${CW:-8}" CSLOT="${CSLOT:-none}" FOFF="${FOFF:-24}" WALL="${WALL:-}" S="${S:-3000}" \
  SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" timeout 30 /data/local/tmp/argsloop > /data/local/tmp/stop.log 2>&1
grep -a -E 'ACP3|LOAD|THR |CMD:|PFD@|q[0-9]|reply=|mmap' /data/local/tmp/stop.log | head -34
echo "== HAL 原话"
logcat -d -s AHAL_Module_QTI AHAL_Stream_QTI AHAL_StreamOut_QTI AHAL_StreamOut_MI 2>/dev/null | tail -12
echo "== 崩溃计数 $(logcat -d | grep -ac 'Fatal signal.*argsloop')"
setprop ctl.start audioserver
sleep 3
echo "audioserver 恢复=$(getprop init.svc.audioserver)"
