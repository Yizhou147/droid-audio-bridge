#!/system/bin/sh
# 静默探针：读/写 IModule 的 vendor 参数（事务码 33 get / 34 set），不开流、不出声。
# 为什么需要它：下半对喇叭（bl/br）的增益是 MI 层在收到 speaker 的 vendor 参数时才开的
#   （MiModulePrimary::setVendorParameters：speaker_number = default/top/bottom/tl-spk/tr-spk/bl-spk/br-spk，
#    配套 "set gain to 817: 0db / 785: -4db / 721: -12db / 55 / 49"；
#    框架放媒体时推的是 audio_volume_stream_music_device_speaker=<index>）。
#
# 用法（root）：
#   sh halvp.sh get "audio_volume_stream_music_device_speaker"
#   sh halvp.sh set "audio_volume_stream_music_device_speaker" 24:11     # poke=对象内偏移:值
# 看 HAL 侧反应：logcat -d | grep -a "setVendorParameters parameters id"
set -u
MODE="${1:-get}"
IDS="${2:-audio_volume_stream_music_device_speaker}"
POKE="$3"
D=/data/local/tmp
[ -x "$D/argsloop" ] || { echo "缺 $D/argsloop" >&2; exit 3; }

VP=1 VPCAP=1 VPIDS="$IDS" VCNT="${VCNT:-1}" VPRST=1 \
  AUTO=1 TRANSACT=1 GOT=1 \
  SVC=android.hardware.audio.core.IModule/default \
  IN="55;1;8;0;0;L:2048;B;B" \
  sh -c 'if [ -n "$1" ]; then VPPOKE="$1"; export VPPOKE; fi
         if [ "$2" = set ]; then VPSEND=1; export VPSEND; fi
         exec /data/local/tmp/argsloop' _ "$POKE" "$MODE"
