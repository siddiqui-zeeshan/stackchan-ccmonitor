#!/bin/sh
# Host tests for the robot code that has no ESP-IDF in it (Bluetooth packet splitting, pairing rule).
# Needs only the Mac's compiler (xcode-select --install).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
c++ -std=c++20 -O1 -Wall -Wextra -Werror -o "$tmp/frames_test" "$here/frames_test.cpp"
"$tmp/frames_test"
