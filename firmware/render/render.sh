#!/bin/sh
# Draws the robot's faces into server/faces/ for the browser preview, from firmware/app_cc_monitor/pix.h.
#   ./render.sh          redraw them (run after changing a face, then commit server/faces/)
#   ./render.sh --check  fail if server/faces/ no longer matches what pix.h draws
# Needs only the Mac's own compiler (xcode-select --install).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
faces="$here/../../server/faces"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

c++ -std=c++20 -O2 -Wall -Wextra -Werror -o "$tmp/render" "$here/render.cpp" -lz
mkdir "$tmp/out"
"$tmp/render" "$tmp/out"

if [ "${1:-}" = "--check" ]; then
    if diff -r "$tmp/out" "$faces" >/dev/null; then
        echo "server/faces matches pix.h"
    else
        echo "server/faces is out of date: run firmware/render/render.sh and commit server/faces" >&2
        exit 1
    fi
else
    mkdir -p "$faces"
    rm -f "$faces"/*.png
    cp "$tmp/out"/*.png "$faces"/
    ls -l "$faces"
fi
