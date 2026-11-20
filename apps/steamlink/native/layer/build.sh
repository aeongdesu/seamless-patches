#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
OUT_DIR="$REPO_ROOT/resources/steamlink"
OUT_SO="$OUT_DIR/libXrApiLayer_pico_motion_tracker.so"

if [[ -n "${ANDROID_NDK_HOME:-}" && -d "$ANDROID_NDK_HOME" ]]; then
    NDK_DIR="$ANDROID_NDK_HOME"
elif [[ -n "${ANDROID_NDK_LATEST_HOME:-}" && -d "$ANDROID_NDK_LATEST_HOME" ]]; then
    NDK_DIR="$ANDROID_NDK_LATEST_HOME"
elif [[ -n "${ANDROID_NDK_ROOT:-}" && -d "$ANDROID_NDK_ROOT" ]]; then
    NDK_DIR="$ANDROID_NDK_ROOT"
elif [[ -n "${ANDROID_HOME:-}" && -d "$ANDROID_HOME/ndk" ]]; then
    NDK_DIR="$(find "$ANDROID_HOME/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
else
    echo "Error: set ANDROID_NDK_HOME or ANDROID_HOME with an installed NDK" >&2
    exit 1
fi

HOST_TAG="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
TOOLCHAIN="$NDK_DIR/toolchains/llvm/prebuilt/$HOST_TAG/bin"
CXX="$TOOLCHAIN/aarch64-linux-android29-clang++"
STRIP="$TOOLCHAIN/llvm-strip"

if [[ ! -x "$CXX" ]]; then
    echo "Error: compiler not found at $CXX" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

"$CXX" \
    -std=c++17 \
    -O2 \
    -fPIC \
    -shared \
    -fno-exceptions \
    -fno-rtti \
    -ffunction-sections \
    -fdata-sections \
    -fvisibility=hidden \
    -fvisibility-inlines-hidden \
    -Wall -Wextra -Werror \
    -static-libstdc++ \
    -Wl,-soname,libXrApiLayer_pico_motion_tracker.so \
    -Wl,--gc-sections \
    -Wl,--exclude-libs,ALL \
    -I"$SCRIPT_DIR/include" \
    -I"$SCRIPT_DIR/../protocol" \
    "$SCRIPT_DIR/src/layer.cpp" \
    -llog \
    -o "$OUT_SO"

"$STRIP" --strip-unneeded "$OUT_SO"
ls -lh "$OUT_SO"
