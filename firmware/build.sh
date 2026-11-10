#!/bin/sh
# Build (and optionally flash) StackChan firmware with the CC Monitor app.
#
#   ./build.sh <robot ws(s) URL> <ROBOT_TOKEN>            # build only
#   ./build.sh <robot ws(s) URL> <ROBOT_TOKEN> --flash    # build and flash over USB
#
# URL example: ws://192.168.1.7:8810/<ROBOT_SECRET>/cc/robot   (or wss://your-host/<ROBOT_SECRET>/cc/robot)
# Needs: git, python3, Docker (ESP-IDF image), and for --flash: pip install esptool.
# Base: official m5stack/StackChan firmware v1.6.2 (commit 28b9f0c) + three small patches:
#   stackchan.patch  registers the app; launcher self-update disabled
#   xiaozhi.patch    voice assistant self-update disabled; audio decode priority fix (stutter)
set -eu
[ $# -ge 2 ] || { echo "usage: $0 <robot URL> <ROBOT_TOKEN> [--flash]"; exit 1; }
URL=$1 TOKEN=$2 FLASH=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/.work}
BASE_COMMIT=28b9f0c

if [ ! -d "$WORK/StackChan" ]; then
  mkdir -p "$WORK"
  git clone https://github.com/m5stack/StackChan.git "$WORK/StackChan"
  git -C "$WORK/StackChan" checkout -q "$BASE_COMMIT"
  (cd "$WORK/StackChan/firmware" && python3 fetch_repos.py)   # dependencies + upstream xiaozhi patch
  git -C "$WORK/StackChan" apply "$HERE/stackchan.patch"
  git -C "$WORK/StackChan/firmware/xiaozhi-esp32" apply "$HERE/xiaozhi.patch"
fi

APP="$WORK/StackChan/firmware/main/apps/app_cc_monitor"
mkdir -p "$APP"
cp "$HERE"/app_cc_monitor/app_cc_monitor.cpp "$HERE"/app_cc_monitor/app_cc_monitor.h "$APP/"
umask 077
printf '#pragma once\n#define CC_MONITOR_URL "%s"\n#define CC_MONITOR_TOKEN "%s"\n' "$URL" "$TOKEN" > "$APP/cc_monitor_config.h"
touch "$WORK/StackChan/firmware/main/CMakeLists.txt"   # pick up new source files

docker run --rm -v "$WORK/StackChan/firmware":/project -w /project espressif/idf:v6.1 idf.py build

if [ "$FLASH" = "--flash" ]; then
  PORT=${PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}
  [ -n "$PORT" ] || { echo "robot not found on USB"; exit 1; }
  cd "$WORK/StackChan/firmware/build"
  python3 -m esptool --chip esp32s3 -p "$PORT" -b 460800 --before default-reset --after hard-reset write-flash @flash_args
fi
echo "done"
