#!/bin/sh
# NDK 交叉编译 audio-probe（M0）。本机 arm64 无 NDK → 实际由 CI 构建；
# 有 NDK 的机器上 sh build.sh 出同样产物。产物：out/audio-probe
set -eu

NDK="${NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}}"
if [ -z "$NDK" ]; then
    echo "需要 NDK：export NDK=/path/to/android-ndk-r27（或设 ANDROID_NDK_HOME）" >&2
    exit 1
fi

OS=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$OS" in
    mingw*|msys*|windows*) HOST_TAG="windows-x86_64" ;;
    darwin)                HOST_TAG="darwin-x86_64" ;;
    *)                     HOST_TAG="linux-x86_64" ;;
esac

# 不链 -lbinder_ndk：runner 那份 NDK 的 stub 不导出 AServiceManager_*，运行时 dlopen（同 bthci-bridge）。
CC="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android33-clang++"
[ -x "$CC" ] || { echo "找不到编译器：$CC" >&2; exit 1; }

mkdir -p out
"$CC" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -fPIE -pie -static-libstdc++ -Wl,--exclude-libs,ALL \
    src/audio-probe.cpp -o out/audio-probe -llog
CC_C="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android33-clang"
"$CC_C" -std=gnu11 -O2 -Wall -fPIE -pie \
    src/audio-probe-raw.c -o out/audio-probe-raw
CC_C="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android33-clang"
# DT_NEEDED 链接版：用真设备库当链接桩（运行时加载系统原生同名库）
"$CC_C" -std=gnu11 -O2 -Wall -fPIE -pie \
    src/audio-probe-dt.c -o out/audio-probe-dt \
    -Ldevice-libs -lbinder -lutils \
    -Wl,--allow-shlib-undefined -Wl,-rpath,/system/lib64

"$CC_C" -std=gnu11 -O2 -Wall -fPIE -pie \
    src/audio-probe-ndk.c -o out/audio-probe-ndk

# AAudio 捷径探针（纯 NDK 公共库，运行时 dlopen libaaudio）
"$CC_C" -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -fPIE -pie \
    src/aaudio-probe.c -o out/aaudio-probe

# 桥本体：FIFO/环回TCP → AAudio
"$CC_C" -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -fPIE -pie \
    src/aa-bridge.c -o out/aa-bridge

# 地面真值 dumper：调平台自己的 Arguments::writeToParcel 倒字节
"$CC_C" -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -fPIE -pie \
    src/argsdump.c -o out/argsdump

# 让平台自己打包 openOutputStream（构造 BpModule 直调）
"$CC_C" -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -fPIE -pie \
    src/hp-open.c -o out/hp-open

# 轮内 activity 服务桩（让 audioserver 走完 onFirstRef 的 waitForService("activity")）
"$CC_C" -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -fPIE -pie \
    src/activity-stub.c -o out/activity-stub
ls -l out/
