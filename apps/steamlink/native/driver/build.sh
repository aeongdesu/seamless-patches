#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/bin/win64"
OUT_DLL="$OUT_DIR/driver_seamless_pico_mt.dll"

if [[ -n "${MINGW_CXX:-}" ]]; then
    CXX="$MINGW_CXX"
elif [[ -x "$HOME/.local/llvm-mingw/bin/x86_64-w64-mingw32-clang++" ]]; then
    CXX="$HOME/.local/llvm-mingw/bin/x86_64-w64-mingw32-clang++"
elif command -v x86_64-w64-mingw32-clang++ >/dev/null 2>&1; then
    CXX="$(command -v x86_64-w64-mingw32-clang++)"
elif command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    CXX="$(command -v x86_64-w64-mingw32-g++)"
else
    echo "Error: install llvm-mingw or mingw-w64 (x86_64-w64-mingw32-clang++ / g++)" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

"$CXX" \
    -std=c++17 \
    -O2 \
    -shared \
    -Wall -Wextra -Werror \
    -Wno-unused-parameter \
    -static \
    -static-libgcc \
    -static-libstdc++ \
    -I"$SCRIPT_DIR/include" \
    -I"$SCRIPT_DIR/../protocol" \
    "$SCRIPT_DIR/src/driver_main.cpp" \
    "$SCRIPT_DIR/src/pico_tracker_device.cpp" \
    -lws2_32 \
    -o "$OUT_DLL"

ls -lh "$OUT_DLL"
