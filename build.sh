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
    -fPIE -pie -static-libstdc++ \
    src/audio-probe.cpp -o out/audio-probe -llog
ls -l out/
