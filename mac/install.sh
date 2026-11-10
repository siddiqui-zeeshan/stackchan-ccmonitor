#!/bin/sh
# Install cc-watch as a per-user LaunchAgent. Usage: ./install.sh <server ws(s) URL> <token>
#   ./install.sh wss://monitor.example.com/cc/mac MY_TOKEN
# Uninstall: ./install.sh --uninstall
set -eu
LABEL=dev.stackchan.cc-watch
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
BIN="$HOME/Applications/cc-watch"

if [ "${1:-}" = "--uninstall" ]; then
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  rm -f "$PLIST" "$BIN"
  echo "cc-watch removed (config kept in ~/.config/cc-watch)"
  exit 0
fi

[ $# -eq 2 ] || { echo "usage: $0 <server URL> <token>"; exit 1; }
cd "$(dirname "$0")"
# Prefer the Rust build (~1.8 MB footprint); fall back to the Swift one (~19 MB).
if command -v cargo >/dev/null; then
  (cd ../mac-rs && cargo build --release -q) && SRC=../mac-rs/target/release/cc-watch
elif [ -x ../mac-rs/target/release/cc-watch ]; then
  SRC=../mac-rs/target/release/cc-watch
else
  [ -x ./cc-watch ] || swiftc -O -o cc-watch cc-watch.swift
  SRC=./cc-watch
fi
mkdir -p "$HOME/Applications" "$HOME/.config/cc-watch" "$HOME/Library/LaunchAgents"
cp "$SRC" "$BIN"
umask 077
printf '{"server":"%s","token":"%s","ghostty":true}\n' "$1" "$2" > "$HOME/.config/cc-watch/config.json"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$BIN</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Background</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/cc-watch.log</string>
</dict></plist>
EOF
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "cc-watch installed; log: ~/Library/Logs/cc-watch.log"
