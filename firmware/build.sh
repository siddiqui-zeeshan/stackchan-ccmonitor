#!/bin/sh
# Build (and optionally flash) StackChan firmware with the CC Monitor app.
#
#   ./build.sh <robot ws(s) URL> <ROBOT_TOKEN> [--flash]   # Wi-Fi build: the robot talks to the server
#   ./build.sh --ble [--flash] [--package <zip>]          # Bluetooth build: display only, talks to cc-ble on
#                                                         # the paired Mac (see the README); --package zips the
#                                                         # flash files for setup.sh / a release
#
# URL example: ws://192.168.1.7:8810/<ROBOT_SECRET>/cc/robot   (or wss://your-host/<ROBOT_SECRET>/cc/robot)
# Needs: git, python3, Docker (ESP-IDF image), and for --flash: pip install esptool.
# Base: official m5stack/StackChan firmware v1.6.2 (commit 28b9f0c) + small patches:
#   stackchan.patch      registers the app; launcher self-update disabled
#   xiaozhi.patch        voice assistant self-update disabled; audio decode priority fix (stutter)
#   stackchan-ble.patch  (Bluetooth build only) opens CC Monitor at boot instead of the stock setup; stock
#                        Bluetooth services off
# The two builds use separate trees (.work and .work-ble), so building one never changes the other.
set -eu
usage() { echo "usage: $0 <robot URL> <ROBOT_TOKEN> [--flash]  |  $0 --ble [--flash] [--package <zip>]"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
BLE=0 FLASH= PACKAGE= URL= TOKEN=
if [ "${1:-}" = "--ble" ]; then
  BLE=1; shift
  while [ $# -gt 0 ]; do
    case $1 in
      --flash) FLASH=--flash ;;
      --package) [ $# -ge 2 ] || usage; PACKAGE=$2; shift ;;
      *) usage ;;
    esac
    shift
  done
  WORK=${WORK:-$HERE/.work-ble}
else
  [ $# -ge 2 ] || usage
  URL=$1 TOKEN=$2 FLASH=${3:-}
  WORK=${WORK:-$HERE/.work}
fi
BASE_COMMIT=28b9f0c

if [ ! -d "$WORK/StackChan" ]; then
  mkdir -p "$WORK"
  git clone https://github.com/m5stack/StackChan.git "$WORK/StackChan"
  git -C "$WORK/StackChan" checkout -q "$BASE_COMMIT"
  (cd "$WORK/StackChan/firmware" && python3 fetch_repos.py)   # dependencies + upstream xiaozhi patch
  git -C "$WORK/StackChan" apply "$HERE/stackchan.patch"
  git -C "$WORK/StackChan/firmware/xiaozhi-esp32" apply "$HERE/xiaozhi.patch"
  if [ "$BLE" = 1 ]; then git -C "$WORK/StackChan" apply "$HERE/stackchan-ble.patch"; fi
fi

FW="$WORK/StackChan/firmware"
APP="$FW/main/apps/app_cc_monitor"
mkdir -p "$APP"
for f in app_cc_monitor.cpp app_cc_monitor.h pix.h frames.h cc_ble.h cc_ble.cpp; do
  cp "$HERE/app_cc_monitor/$f" "$APP/"
done
umask 077
if [ "$BLE" = 1 ]; then
  printf '#pragma once\n#define CC_MONITOR_BLE 1\n' > "$APP/cc_monitor_config.h"
else
  printf '#pragma once\n#define CC_MONITOR_URL "%s"\n#define CC_MONITOR_TOKEN "%s"\n' "$URL" "$TOKEN" > "$APP/cc_monitor_config.h"
fi
touch "$FW/main/CMakeLists.txt"   # pick up new source files

idf() { docker run --rm -v "$FW":/project -w /project espressif/idf:v6.1 idf.py "$@"; }

if [ "$BLE" = 1 ]; then
  # Bluetooth build: keep the Mac's bond across restarts. Room for 2 bonds: ESP-IDF's bond store doesn't
  # compile with 1; cc_ble.cpp keeps only the newest pairing.
  [ -f "$FW/sdkconfig" ] || idf reconfigure
  set_cfg() {
    grep -v -e "^$1=" -e "^# $1 is not set" "$FW/sdkconfig" > "$FW/sdkconfig.tmp"
    echo "$1=$2" >> "$FW/sdkconfig.tmp"
    mv "$FW/sdkconfig.tmp" "$FW/sdkconfig"
  }
  set_cfg CONFIG_BT_NIMBLE_NVS_PERSIST y
  set_cfg CONFIG_BT_NIMBLE_MAX_BONDS 2
  set_cfg CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1   # the paired Mac only
fi

idf build

if [ -n "$PACKAGE" ]; then
  case $PACKAGE in /*) ;; *) PACKAGE="$PWD/$PACKAGE" ;; esac
  rm -f "$PACKAGE"
  (cd "$FW/build" && zip -q "$PACKAGE" flash_args $(awk 'NR>1 {print $2}' flash_args))
  echo "packaged: $PACKAGE"
fi

if [ "$FLASH" = "--flash" ]; then
  PORT=${PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}
  [ -n "$PORT" ] || { echo "robot not found on USB"; exit 1; }
  cd "$FW/build"
  python3 -m esptool --chip esp32s3 -p "$PORT" -b 460800 --before default-reset --after hard-reset write-flash @flash_args
fi
echo "done"
