#!/bin/sh
# 逐个试框架创建好的输出 portConfig，看哪一个能被我们直开、并且 HAL 会不会打 start。
# 判据不看耳朵：① 回包字节数（533 = 成功，13 = 被拒）；② HAL 自己的日志。
# 零采样数据，不会出声。
for ID in 54 62 57 60 61 56; do
    logcat -c
    DBG=1 GOT=1 AUTO=1 TRANSACT=1 \
      SVC=android.hardware.audio.core.IModule/default \
      IN="$ID;1;8;0;0;L:2048;B;B" timeout 20 /data/local/tmp/argsloop \
      > /data/local/tmp/t.log 2>&1
    R=$(grep -a 'GOT\] reply=' /data/local/tmp/t.log | head -1)
    F=$(ls /proc/$(pidof audiohalservice.qti)/fd 2>/dev/null | wc -l)
    echo "== id=$ID  $R  HALfd=$F"
    logcat -d 2>/dev/null | grep -a "AHAL_" | grep -av getAudioPorts | head -5
done
