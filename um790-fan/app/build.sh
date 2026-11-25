#!/bin/sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
APP="$ROOT/build/UM790FanBar.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
clang -fobjc-arc -O2 -Wall -Wextra -I"$ROOT" \
  -framework Cocoa -framework IOKit \
  "$HERE/UM790FanBar.m" -o "$APP/Contents/MacOS/UM790FanBar"
cp "$HERE/Info.plist" "$APP/Contents/Info.plist"
codesign --force --sign - "$APP"
echo "built $APP"
