#!/bin/sh
# Tests for ../setup.sh, using --dry-run against a throwaway HOME (writes config files, builds/flashes/loads nothing).
#   sh tests/test_setup.sh
set -u
repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

# A stand-in firmware package: flash_args plus the file it names.
mkdir "$tmp/fw"
printf -- '--flash-mode dio --flash-freq 80m --flash-size 16MB\n0x20000 stack-chan.bin\n' > "$tmp/fw/flash_args"
printf 'not really firmware' > "$tmp/fw/stack-chan.bin"
(cd "$tmp/fw" && zip -q ../fw.zip flash_args stack-chan.bin)
sha=$(shasum -a 256 "$tmp/fw.zip" | awk '{print $1}')

plist_get() {   # plist_get <file> <EnvironmentVariables key>
  /usr/libexec/PlistBuddy -c "Print :EnvironmentVariables:$2" "$1" 2>/dev/null
}

run() {   # run <home> <TZ> [extra env...]: dry-run setup
  h=$1 tz=$2
  shift 2
  mkdir -p "$h"
  env HOME="$h" TZ="$tz" CC_FIRMWARE_SHA256="$sha" "$@" sh "$repo/setup.sh" --dry-run --firmware "$tmp/fw.zip" \
    > "$h.out" 2>&1
}

# --- a first run writes matching configs
h="$tmp/home1"
if ! run "$h" UTC-1; then
  fail "dry run exited non-zero:"
  cat "$h.out" >&2
fi
srv="$h/Library/LaunchAgents/dev.stackchan.cc-monitor.plist"
ble="$h/Library/LaunchAgents/dev.stackchan.cc-ble.plist"
for f in "$srv" "$ble" "$h/.config/cc-monitor/ble.json" "$h/.config/cc-watch/config.json"; do
  [ -f "$f" ] || fail "missing $f"
done
[ "$(plist_get "$srv" LISTEN_HOST)" = 127.0.0.1 ] || fail "server must listen on 127.0.0.1 only"
[ "$(plist_get "$srv" LISTEN_PORT)" = 8810 ] || fail "server port is not 8810"
[ -z "$(plist_get "$srv" GEMINI_API_KEY)" ] || fail "GEMINI_API_KEY must be empty (no Talk)"
[ "$(plist_get "$srv" TZ_OFFSET_MINUTES)" = 60 ] || fail "TZ_OFFSET_MINUTES for +0100 is not 60"
for f in "$srv" "$h/.config/cc-monitor/ble.json" "$h/.config/cc-watch/config.json"; do
  [ "$(stat -f %Lp "$f" 2>/dev/null)" = 600 ] || fail "$f is not mode 600"
done

mac_token=$(plist_get "$srv" MAC_TOKENS)
robot_token=$(plist_get "$srv" ROBOT_TOKEN)
robot_secret=$(plist_get "$srv" ROBOT_SECRET)
for t in "$mac_token" "$robot_token" "$robot_secret"; do
  echo "$t" | grep -Eq '^[0-9a-f]{32,}$' || fail "token '$t' is not 32+ hex characters"
done
[ "$mac_token" != "$robot_token" ] || fail "Mac and robot tokens are the same"
python3 - "$h" "$mac_token" "$robot_token" "$robot_secret" <<'EOF' || fail "configs don't agree with the server's tokens"
import json, sys
h, mac, robot, secret = sys.argv[1:]
w = json.load(open(f"{h}/.config/cc-watch/config.json"))
b = json.load(open(f"{h}/.config/cc-monitor/ble.json"))
assert w == {"server": "ws://127.0.0.1:8810/cc/mac", "token": mac, "ghostty": False}, w
assert b["server"] == f"ws://127.0.0.1:8810/{secret}/cc/robot", b
assert b["token"] == robot, b
EOF

# --- a second run keeps the same tokens (an already paired robot keeps working)
run "$h" UTC-1 || fail "second dry run exited non-zero"
[ "$(plist_get "$srv" ROBOT_TOKEN)" = "$robot_token" ] || fail "re-run changed the robot token"

# --- quiet hours follow the Mac's clock
h2="$tmp/home2"
run "$h2" UTC+5:30 || fail "dry run (UTC+5:30) exited non-zero"
[ "$(plist_get "$h2/Library/LaunchAgents/dev.stackchan.cc-monitor.plist" TZ_OFFSET_MINUTES)" = -330 ] \
  || fail "TZ_OFFSET_MINUTES for -0530 is not -330"

# --- a firmware package that doesn't match the checksum is refused
h3="$tmp/home3"
if run "$h3" UTC-1 CC_FIRMWARE_SHA256=0000000000000000000000000000000000000000000000000000000000000000; then
  fail "a firmware checksum mismatch did not stop setup"
fi
grep -q "checksum" "$h3.out" || fail "the checksum failure doesn't say so"

if [ "$fails" -gt 0 ]; then
  echo "$fails failed" >&2
  exit 1
fi
echo ok
