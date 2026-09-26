#!/bin/sh
# 我们那条流的工作线程（write_db）到底在等哪个 futex？
# /proc/<pid>/task/<tid>/syscall 给出行 syscall 的参数（第一个就是 uaddr），
# 再用 HAL 自己的 maps 把它换算成"某块 ashmem 内的偏移" —— 那才是真正的通知字位置。
P=$(pidof audiohalservice.qti)
[ -z "$P" ] && { echo "没有 audiohalservice.qti"; exit 1; }
for T in /proc/$P/task/*; do
    N=$(cat "$T/comm" 2>/dev/null)
    case "$N" in
        write*|*db*)
            SC=$(cat "$T/syscall" 2>/dev/null)
            echo "tid=$(basename $T) comm=$N syscall: $SC"
            case "$SC" in
                98*|65*)  # 65=futex(FUTEX_WAIT_PRIVATE), 98=futex_waitv-ish
                    A=$(echo "$SC" | awk '{print $2}')
                    AA=$(printf '%d' "$A" 2>/dev/null) || continue
                    grep -a -i "ashmem\|memfd" /proc/$P/maps 2>/dev/null | while read -r L; do
                        S=$(echo "$L" | cut -d- -f1); E=$(echo "$L" | cut -d' ' -f1 | cut -d- -f2)
                        Sv=$(printf '%d' "0x$S" 2>/dev/null); Ev=$(printf '%d' "0x$E" 2>/dev/null)
                        [ -z "$Sv" ] && continue
                        if [ "$AA" -ge "$Sv" ] && [ "$AA" -lt "$Ev" ]; then
                            POF=$(echo "$L" | awk '{print $3}')
                            Pv=$(printf '%d' "0x$POF" 2>/dev/null)
                            OFF=$(( AA - Sv ))
                            echo "    → 命中映射 $S-$E 页内偏移(十进制) $OFF  文件偏移 $POF  ⇒ 区内偏移 $((OFF + Pv))"
                            echo "    $(echo "$L" | awk '{print $6}')"
                        fi
                    done
                    ;;
            esac
            ;;
    esac
done
