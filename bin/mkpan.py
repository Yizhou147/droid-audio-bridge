#!/usr/bin/env python3
"""生成"左右声道轮流"的 16-bit/48k 立体声诊断信号（写 wav，也可 --raw 出裸流喂 sink）。

一段里依次是：静音 → 只左 → 静音 → 只右 → 静音 → 左右同相，共两轮。
用途：判断当前这条链有没有真的把 L/R 分到两侧喇叭（接管轮里 rotation=0 时右对不响）。
"""
import argparse
import math
import os
import struct
import wave

SR = 48000
AMP = float(os.environ.get("PAN_AMP", "0.10"))   # 幅度（0.10 = -20 dBFS）
SEQ = [("sil", 0.6), ("L", 2.0), ("sil", 0.6), ("R", 2.0), ("sil", 0.6), ("B", 2.0)]


def frames():
    for _round in range(2):
        for kind, dur in SEQ:
            n = int(SR * dur)
            for i in range(n):
                s = int(AMP * 32767 * math.sin(2 * math.pi * 440.0 * i / SR))
                yield (s, 0) if kind == "L" else \
                      (0, s) if kind == "R" else \
                      (s, s) if kind == "B" else (0, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--amp", type=float, default=None, help="覆盖 PAN_AMP")
    ap.add_argument("--raw", action="store_true", help="输出 s16le 裸流（喂 HAL sink，不要 wav 头）")
    ap.add_argument("out")
    a = ap.parse_args()
    global AMP
    if a.amp is not None: AMP = a.amp
    if a.raw:
        with open(a.out, "wb") as f:
            for l, r in frames():
                f.write(struct.pack("<hh", l, r))
    else:
        with wave.open(a.out, "wb") as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
            w.writeframes(b"".join(struct.pack("<hh", l, r) for l, r in frames()))
    print(a.out)


main()
