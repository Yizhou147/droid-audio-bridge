#!/bin/sh
# GOT 抓包 + stream 句柄链。三种力度：
#   （默认）        OBJS=1：只打开流、只看 BpStreamOut 对象布局，一个 stream 事务都不发
#   TX=1            额外对句柄发 getStreamCommon(码 1)
#   TX=1 MMAP=1     再往下：解析 IStreamCommon 并对它发 createMmapBuffer(码 8) 找共享内存 fd
# 每次都要先重启 HAL —— port 63 被上一轮留下的流占着，不重启第二次 open 必被拒。
# 全程零采样数据：只 open / 只查询，不写一帧，不会出声。
set -u
M=/data/local/tmp/argsloop.log
setprop ctl.restart vendor.audio-hal-aidl
sleep 6
H=$(pidof audiohalservice.qti); echo "底 fd=$(ls /proc/$H/fd | wc -l)"
MODE=OBJS=1
[ "${TX:-0}" = 1 ] && MODE="STREAM=1"
[ "${TX:-0}" = 1 ] && [ "${MMAP:-0}" = 1 ] && MODE="STREAM=1 MMAP=1"
env DBG=1 $MODE AUTO=1 TRANSACT=1 GOT=1 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="63;1;8;0;0;L:2048;B;B" timeout 25 /data/local/tmp/argsloop > "$M" 2>&1
grep -aE 'GOT\] reply|RETSHEAP|cand ret\+0|AIBinder 在|==> getStream|STREAM|VERDICT|M2a|MMAP|createMmap|readFromParcel rc|未成|TRANSACT st' "$M" | head -40
H=$(pidof audiohalservice.qti); echo "尾 fd=$(ls /proc/$H/fd | wc -l)"
