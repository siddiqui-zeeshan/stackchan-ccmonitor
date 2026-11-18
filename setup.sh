#!/bin/sh
# Set up the Bluetooth StackChan on this Mac (branch mac-direct-ble): no server elsewhere, display only.
#
#   ./setup.sh                    install everything, flash the robot (plugged in by USB), pair with it
#   ./setup.sh --pair             pair again (robot plugged in by USB and restarted in the last 2 minutes)
#   ./setup.sh --uninstall        remove the programs and LaunchAgents (keeps ~/.config/cc-monitor)
#   ./setup.sh --firmware <zip>   flash this package (from firmware/build.sh --ble --package) instead of the release
#   ./setup.sh --dry-run          write the config files only: no build, download, flash, pairing or LaunchAgents
#
# On this Mac afterwards:
#   dev.stackchan.cc-monitor  the server (server/cc_monitor.py), on 127.0.0.1:8810 only
#   dev.stackchan.cc-watch    reports Claude Code sessions to it (mac/install.sh, Ghostty lookups off)
#   dev.stackchan.cc-ble      carries messages between the server and the robot over Bluetooth (mac/ble)
# Logs: ~/Library/Logs/cc-monitor.log, cc-watch.log, cc-ble.log
set -eu
REPO=$(cd "$(dirname "$0")" && pwd)
RELEASE_URL=https://github.com/siddiqui-zeeshan/stackchan-ccmonitor/releases/download/ble-v1/stackchan-cc-ble.zip
SUPPORT="$HOME/Library/Application Support/cc-monitor"
CONF="$HOME/.config/cc-monitor"
AGENTS="$HOME/Library/LaunchAgents"
LOGS="$HOME/Library/Logs"
BIN="$HOME/Applications/cc-ble"
VENV="$SUPPORT/venv"
PORT=8810
SERVER_LABEL=dev.stackchan.cc-monitor
BLE_LABEL=dev.stackchan.cc-ble
UID_=$(id -u)

DRY=0 MODE=install FIRMWARE=
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) DRY=1 ;;
    --pair) MODE=pair ;;
    --uninstall) MODE=uninstall ;;
    --firmware) [ $# -ge 2 ] || { echo "--firmware needs a zip"; exit 1; }; FIRMWARE=$2; shift ;;
    *) sed -n '2,13p' "$0"; exit 1 ;;
  esac
  shift
done

say() { printf '%s\n' "$*"; }
agent_load() { launchctl bootout "gui/$UID_/$1" 2>/dev/null || true; launchctl bootstrap "gui/$UID_" "$AGENTS/$1.plist"; }
agent_stop() { launchctl bootout "gui/$UID_/$1" 2>/dev/null || true; }

if [ "$MODE" = uninstall ]; then
  agent_stop "$BLE_LABEL"
  agent_stop "$SERVER_LABEL"
  rm -f "$AGENTS/$BLE_LABEL.plist" "$AGENTS/$SERVER_LABEL.plist" "$BIN"
  rm -rf "$SUPPORT"
  sh "$REPO/mac/install.sh" --uninstall
  say "Removed. Your settings stay in $CONF (delete it to forget the pairing and tokens)."
  exit 0
fi

pair() {
  agent_stop "$BLE_LABEL"
  say ""
  say "Pairing. The robot must be plugged into this Mac by USB, with CC Monitor started in the last 2 minutes"
  say "(it shows \"Ready to pair\"; if not, restart it). When macOS asks, type the code the robot shows."
  "$BIN" --pair
  agent_load "$BLE_LABEL"
  say "Paired. The robot can stay plugged in for power or be unplugged; it uses Bluetooth either way."
}

if [ "$MODE" = pair ]; then
  [ -x "$BIN" ] || { say "cc-ble isn't installed: run ./setup.sh first"; exit 1; }
  pair
  exit 0
fi

# ---------------------------------------------------------------- checks
if [ "$DRY" = 0 ]; then
  xcode-select -p >/dev/null 2>&1 || { say "Run xcode-select --install first, then run this again."; exit 1; }
  agent_stop "$SERVER_LABEL"   # a re-run: free our own port first
  if busy=$(lsof -nP -iTCP:$PORT -sTCP:LISTEN 2>/dev/null | awk 'NR == 2 {print $1 " (pid " $2 ")"}') && [ -n "$busy" ]; then
    say "Port $PORT is in use by $busy. Stop it and run this again."
    exit 1
  fi
fi

# ---------------------------------------------------------------- firmware package (checked before anything else)
want_sha=${CC_FIRMWARE_SHA256:-$(awk '{print $1; exit}' "$REPO/firmware/ble-firmware.sha256")}
mkdir -p "$SUPPORT"
if [ -z "$FIRMWARE" ]; then
  FIRMWARE="$SUPPORT/stackchan-cc-ble.zip"
  say "Downloading the robot firmware..."
  curl -fsSL -o "$FIRMWARE" "$RELEASE_URL"
fi
got_sha=$(shasum -a 256 "$FIRMWARE" | awk '{print $1}')
if [ "$got_sha" != "$want_sha" ]; then
  say "The firmware package doesn't match the expected checksum; not using it."
  say "  expected $want_sha"
  say "  got      $got_sha"
  exit 1
fi

# ---------------------------------------------------------------- tokens (kept across re-runs)
mkdir -p "$CONF" "$AGENTS" "$LOGS"
chmod 700 "$CONF"
umask 077
if [ ! -f "$CONF/server.env" ]; then
  {
    echo "MAC_TOKEN=$(openssl rand -hex 16)"
    echo "ROBOT_TOKEN=$(openssl rand -hex 16)"
    echo "ROBOT_SECRET=$(openssl rand -hex 16)"
  } > "$CONF/server.env"
fi
MAC_TOKEN=$(sed -n 's/^MAC_TOKEN=//p' "$CONF/server.env")
ROBOT_TOKEN=$(sed -n 's/^ROBOT_TOKEN=//p' "$CONF/server.env")
ROBOT_SECRET=$(sed -n 's/^ROBOT_SECRET=//p' "$CONF/server.env")

# Quiet hours (the server's QUIET_HOURS, default 22-8) are in this Mac's local time.
z=$(date +%z)   # e.g. +0530
sign=$(printf %s "$z" | cut -c1) hh=$(printf %s "$z" | cut -c2-3) mm=$(printf %s "$z" | cut -c4-5)
TZ_MIN=$(( ${hh#0} * 60 + ${mm#0} ))
if [ "$sign" = "-" ]; then TZ_MIN=$(( -TZ_MIN )); fi

# ---------------------------------------------------------------- server
if [ "$DRY" = 0 ]; then
  say "Setting up the server..."
  [ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
  "$VENV/bin/pip" install -q --disable-pip-version-check -r "$REPO/server/requirements.txt" esptool
  rm -rf "$SUPPORT/server"
  mkdir -p "$SUPPORT/server"
  cp -R "$REPO/server/cc_monitor.py" "$REPO/server/preview.html" "$REPO/server/faces" "$SUPPORT/server/"
fi
cat > "$AGENTS/$SERVER_LABEL.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$SERVER_LABEL</string>
  <key>ProgramArguments</key><array><string>$VENV/bin/python</string><string>$SUPPORT/server/cc_monitor.py</string></array>
  <key>WorkingDirectory</key><string>$SUPPORT/server</string>
  <key>EnvironmentVariables</key><dict>
    <key>LISTEN_HOST</key><string>127.0.0.1</string>
    <key>LISTEN_PORT</key><string>$PORT</string>
    <key>MAC_TOKENS</key><string>$MAC_TOKEN</string>
    <key>ROBOT_TOKEN</key><string>$ROBOT_TOKEN</string>
    <key>ROBOT_SECRET</key><string>$ROBOT_SECRET</string>
    <key>PREVIEW_TOKEN</key><string></string>
    <key>GEMINI_API_KEY</key><string></string>
    <key>TZ_OFFSET_MINUTES</key><string>$TZ_MIN</string>
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Background</string>
  <key>StandardErrorPath</key><string>$LOGS/cc-monitor.log</string>
</dict></plist>
EOF
[ "$DRY" = 1 ] || agent_load "$SERVER_LABEL"

# ---------------------------------------------------------------- cc-watch (sessions -> server), Ghostty lookups off
if [ "$DRY" = 0 ]; then
  say "Setting up cc-watch..."
  GHOSTTY=false sh "$REPO/mac/install.sh" "ws://127.0.0.1:$PORT/cc/mac" "$MAC_TOKEN"
else
  mkdir -p "$HOME/.config/cc-watch"
  printf '{"server":"%s","token":"%s","ghostty":false}\n' "ws://127.0.0.1:$PORT/cc/mac" "$MAC_TOKEN" \
    > "$HOME/.config/cc-watch/config.json"
fi

# ---------------------------------------------------------------- cc-ble (server <-> robot over Bluetooth)
robot=
[ -f "$CONF/ble.json" ] && robot=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("robot",""))' "$CONF/ble.json")
printf '{"server": "ws://127.0.0.1:%s/%s/cc/robot", "token": "%s", "robot": "%s"}\n' \
  "$PORT" "$ROBOT_SECRET" "$ROBOT_TOKEN" "$robot" > "$CONF/ble.json"
cat > "$AGENTS/$BLE_LABEL.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$BLE_LABEL</string>
  <key>ProgramArguments</key><array><string>$BIN</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Background</string>
  <key>StandardErrorPath</key><string>$LOGS/cc-ble.log</string>
</dict></plist>
EOF
if [ "$DRY" = 1 ]; then
  say "Dry run: wrote $CONF, $HOME/.config/cc-watch and the LaunchAgents in $AGENTS; firmware checksum OK."
  exit 0
fi
say "Building cc-ble..."
mkdir -p "$HOME/Applications"
sh "$REPO/mac/ble/build.sh" "$BIN"

# ---------------------------------------------------------------- flash the robot, then pair
rm -rf "$SUPPORT/firmware"
mkdir -p "$SUPPORT/firmware"
unzip -q "$FIRMWARE" -d "$SUPPORT/firmware"
set -- /dev/cu.usbmodem*
flashed=0
if [ ! -e "$1" ]; then
  say "No robot found on USB, so it wasn't flashed. Plug it in and run ./setup.sh again."
elif [ $# -gt 1 ]; then
  say "Several USB serial devices found ($*); unplug all but the robot and run ./setup.sh again."
else
  printf 'Flash the robot on %s? It replaces the firmware on it. [y/N] ' "$1"
  read -r answer
  if [ "$answer" = y ] || [ "$answer" = Y ]; then
    (cd "$SUPPORT/firmware" && "$VENV/bin/python" -m esptool --chip esp32s3 -p "$1" -b 460800 \
      --before default-reset --after hard-reset write-flash @flash_args)
    flashed=1
    say "Flashed. The robot restarts into CC Monitor."
    sleep 8
  fi
fi

if [ "$flashed" = 1 ] || [ -z "$robot" ]; then
  pair
else
  agent_load "$BLE_LABEL"
fi
say ""
say "Done. Logs: $LOGS/cc-monitor.log, cc-watch.log, cc-ble.log. Preview: http://127.0.0.1:$PORT/cc/preview"
