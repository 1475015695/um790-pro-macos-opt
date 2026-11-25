#!/bin/sh
# Builds UM790Fan.kext natively on macOS with MacKernelSDK.
# Usage: ./build.sh [path-to-MacKernelSDK]
set -eu
MKSDK=${1:-${MACKERNELSDK:-../../MacKernelSDK}}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT="$ROOT/build/UM790Fan.kext"
rm -rf "$HERE/build" "$OUT"
mkdir -p "$HERE/build" "$OUT/Contents/MacOS"

CFLAGS="-arch x86_64 -mmacosx-version-min=11.0 -mkernel -fno-builtin -fno-common -nostdinc
  -I$MKSDK/Headers -I$ROOT -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -Os -Wall -Wextra"

clang -x c $CFLAGS -c "$HERE/kmod_info.c" -o "$HERE/build/kmod_info.o"
clang++ $CFLAGS -std=c++17 -fapple-kext -fno-rtti -fno-exceptions \
  -c "$HERE/UM790Fan.cpp" -o "$HERE/build/UM790Fan.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -nostdlib -Xlinker -kext \
  -L"$MKSDK/Library/x86_64" -lkmodc++ -lkmod -lcc_kext \
  "$HERE/build/kmod_info.o" "$HERE/build/UM790Fan.o" \
  -o "$OUT/Contents/MacOS/UM790Fan"
cp "$HERE/Info.plist" "$OUT/Contents/Info.plist"
codesign --force --sign - "$OUT"
echo "built $OUT"
