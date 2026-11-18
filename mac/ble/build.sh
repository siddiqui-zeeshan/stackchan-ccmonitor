#!/bin/sh
# Build cc-ble: ./build.sh <output path>. Embeds Info.plist, which macOS requires before a program may use
# Bluetooth. Needs only the Mac's compiler (xcode-select --install).
set -eu
[ $# -eq 1 ] || { echo "usage: $0 <output path>"; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
swiftc -O "$here/Frames.swift" "$here/Relay.swift" "$here/main.swift" -o "$1" \
  -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker "$here/Info.plist"
