#!/system/bin/sh
# 静默探针：只发 IModule::updateScreenRotation(<n>)，不开流、不出声。
# 用来验证接管轮里我们能不能自己把 PAL 的 device rotation 推起来
# （正常链路是 AudioFlinger setGameParameters("rotation=90") → pal_set_param id 10）。
#
# 用法（root）：sh halrot.sh 1        # 0=竖屏 1=横屏90° 2=180 3=270
# 看效果：      logcat -d | grep -E "pal_set_param|handleDeviceRotationChange|SetOrientationCal"
set -u
ROT="${1:-1}"
D=/data/local/tmp
[ -x "$D/argsloop" ] || { echo "缺 $D/argsloop（CI 产物 push 上来）" >&2; exit 3; }
ROT="$ROT" ROTONLY=1 \
  AUTO=1 TRANSACT=1 STREAM=1 GOT=1 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" \
  exec "$D/argsloop"
