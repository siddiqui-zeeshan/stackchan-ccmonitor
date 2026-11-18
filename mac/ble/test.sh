#!/bin/sh
# Tests for cc-ble's packet splitting and relay rules. Needs only the Mac's compiler (xcode-select --install).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$here"
cp Tests.swift "$tmp/main.swift"   # top-level test code must be in main.swift
swiftc -O Frames.swift Relay.swift "$tmp/main.swift" -o "$tmp/t"
"$tmp/t"
