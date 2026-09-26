#!/bin/sh
# 只读旁观：把音频相关进程里所有 ashmem/memfd 映射的头部 64 字节打出来。
# 用途：框架自己跑一条真流时，FMQ 的 writePos/readPos 会在这些块里跳动 ——
#       跳动的位置和增量就是"权威布局"，不用再猜。
# 不做任何写操作、不发任何事务。
# 用法：sh fmq-snoop.sh [标签]
TAG="${1:-snap}"
for NAME in audiohalservice.qti audioserver; do
    for P in $(pidof "$NAME"); do
        echo "===== $TAG $NAME pid=$P"
        ls -l /proc/$P/fd 2>/dev/null | grep -aiE 'ashmem|memfd' | head -12
        # maps 里带 ashmem/memfd 的映射，逐个读头部
        grep -aiE 'ashmem|memfd' /proc/$P/maps 2>/dev/null | head -12 | while read -r L; do
            A=$(echo "$L" | cut -d- -f1)
            PATH_=$(echo "$L" | awk '{print $6}')
            OFF=$(printf '%d' "0x$A" 2>/dev/null) || continue
            echo "  映射 $A $PATH_"
            dd if=/proc/$P/mem bs=4096 skip=$((OFF / 4096)) count=1 2>/dev/null \
                | od -An -tu4 -N 64 | head -4
        done
    done
done
