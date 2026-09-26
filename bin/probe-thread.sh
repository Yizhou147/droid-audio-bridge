#!/bin/sh
# 旁观 HAL 里那条流工作线程（write_db / write_spatial…）在"我们改了计数器之后"在干什么：
# 阻塞点 wchan + 它自己累计的 CPU 时间（utime 涨 = 它在跑活 = 消费了我们的数据）。
P=$(pidof audiohalservice.qti)
[ -z "$P" ] && { echo "找不到 audiohalservice.qti"; exit 1; }
echo "hal pid=$P"
for T in $(ls /proc/$P/task); do
    N=$(cat /proc/$P/task/$T/comm 2>/dev/null)
    case "$N" in
        write*|*db*|*spatial*|*pal*|*agm*)
            W=$(cat /proc/$P/task/$T/wchan 2>/dev/null)
            U=$(awk '{print $14+$15}' /proc/$P/task/$T/stat 2>/dev/null)
            S=$(awk '{print $3}' /proc/$P/task/$T/stat 2>/dev/null)
            echo "  tid=$T comm=$N state=$S wchan=$W cpu=$U"
            ;;
    esac
done
